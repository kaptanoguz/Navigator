// Compile-only check that the Phase 5 contracts (walk: streaming, cancellable recursive
// enumeration; search: compiled queries, matcher, ranking, type filter, filter pipeline; index:
// IIndex lifecycle and queries, default volume policy, stable ids and file location; the
// per-tab type filter in TabState; AppConfig index fields; the new CommandIds) are usable as
// documented. Nothing here is linked or run.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/fs.hpp>
#include <navigator/index.hpp>
#include <navigator/layout.hpp>
#include <navigator/search.hpp>
#include <navigator/types.hpp>
#include <navigator/ui.hpp>
#include <navigator/volume.hpp>
#include <navigator/walk.hpp>
#include <navigator/watch.hpp>

namespace navigator::contracts_check {

namespace se = navigator::search;
namespace lay = navigator::layout;

// Stable ids that must never change.
static_assert(static_cast<std::uint32_t>(CommandId::FocusFilter) == 1300);
static_assert(static_cast<std::uint32_t>(CommandId::ClearFilter) == 1301);
static_assert(static_cast<std::uint32_t>(CommandId::ToggleFlatten) == 1302);
static_assert(static_cast<std::uint32_t>(CommandId::FlattenVolume) == 1303);
static_assert(static_cast<std::uint32_t>(CommandId::ToggleTypesPopup) == 1304);
static_assert(static_cast<std::uint32_t>(CommandId::ToggleFuzzy) == 1305);
static_assert(static_cast<std::uint32_t>(CommandId::CancelSearch) == 1306);
static_assert(static_cast<std::uint32_t>(CommandId::RebuildIndex) == 1307);

// Matching vocabulary.
static_assert(se::to_string(se::MatchMode::Substring) == "substring");
static_assert(se::to_string(se::MatchMode::Fuzzy) == "fuzzy");
static_assert(se::kBonusPrefix > se::kBonusWordStart);
static_assert(se::kPenaltyLeadingMax >= se::kPenaltyLeading);
static_assert(se::HighlightRange{1, 3} == se::HighlightRange{1, 3});
static_assert(se::to_string(se::IndexState::Ready) == "ready");
static_assert(se::to_string(se::IndexState::Verifying) == "verifying");

// FNV-1a 64 reference values (file trailer, stable ids).
static_assert(se::fnv1a64("") == 0xcbf29ce484222325ULL);
static_assert(se::fnv1a64("a") == 0xaf63dc4c8601ec8cULL);

// Walk vocabulary.
// (WalkOptions holds a Path, so its defaults are checked at run time by tests/core.)
bool walk_defaults() {
    const fs::WalkOptions o;
    return o.max_depth == fs::kUnlimitedDepth && !o.follow_symlinks && !o.cross_filesystems &&
           !o.show_hidden && o.skip_unreadable && o.metadata && o.max_entries > 0;
}
static_assert(std::is_move_constructible_v<fs::Walker> &&
              !std::is_copy_constructible_v<fs::Walker>);

// Values.
static_assert(std::is_copy_constructible_v<se::Matcher>);
static_assert(std::is_default_constructible_v<se::TypeFilter>);
static_assert(!std::is_copy_constructible_v<se::IIndex>);
static_assert(std::is_copy_constructible_v<se::IndexStatus>);

// A flatten worker: walk, match each batch, keep the directory table.
std::size_t use_walker(const Path& root, const std::stop_token& stop) {
    fs::WalkOptions options;
    options.root = root;
    options.show_hidden = false;
    options.max_entries = 500'000;
    Result<fs::Walker> walker = fs::Walker::open(options);
    if (!walker) {
        return walker.error().code == ErrorCode::PermissionDenied ? 1U : 0U;
    }
    std::vector<std::string> dirs{std::string()}; // index 0: the root
    std::vector<fs::WalkEntry> flat;
    se::Matcher matcher(se::compile_query("rep 24", se::MatchMode::Fuzzy));
    const se::TypeFilter types;
    std::vector<se::FilterHit> hits;
    fs::WalkBatch batch;
    std::size_t total = 0;
    for (;;) {
        Result<bool> more = walker->next_batch(batch, stop);
        if (!more) {
            total += more.error().code == ErrorCode::Cancelled ? 1U : 0U;
            break;
        }
        if (batch.first_dir == dirs.size()) {
            dirs.insert(dirs.end(), batch.new_dirs.begin(), batch.new_dirs.end());
        }
        if (Result<void> r = se::filter_entries(Span<const fs::WalkEntry>(batch.entries), matcher,
                                                types, hits, stop);
            r) {
            total += hits.size();
        }
        flat.insert(flat.end(), batch.entries.begin(), batch.entries.end());
        if (!*more) {
            break;
        }
    }
    const fs::WalkStats& stats = walker->stats();
    total += static_cast<std::size_t>(stats.dirs_visited + stats.skipped_permission +
                                      stats.skipped_other_fs + stats.skipped_cycles);
    total += stats.truncated ? 1U : 0U;
    total += stats.samples.size();
    for (const fs::WalkEntry& e : flat) {
        total += dirs.at(e.dir).size() + e.depth + e.self_dir;
    }
    return total;
}

// Folder filter on the UI thread, highlights for visible rows, ranking for a flattened view.
std::size_t use_matching(const std::vector<fs::DirEntry>& entries) {
    std::size_t total = 0;
    const se::Query typed = se::compile_query("  İstan ", se::MatchMode::Substring);
    total += typed.empty() ? 0U : typed.terms.size();
    const se::Query more = se::compile_query("  İstanb", se::MatchMode::Substring);
    total += se::narrows(typed, more) ? 1U : 0U;

    se::Matcher matcher(more);
    if (entries.size() <= se::kUiThreadFilterLimit) {
        std::vector<se::FilterHit> hits;
        se::TypeFilter types;
        types.hidden_extensions.emplace_back("tmp");
        se::normalize(types);
        if (se::filter_entries(Span<const fs::DirEntry>(entries), matcher, types, hits)) {
            for (const se::FilterHit& hit : hits) {
                if (std::optional<se::Match> m = matcher.match(entries[hit.index].name)) {
                    for (const se::HighlightRange& r : m->highlights) {
                        total += r.end - r.begin;
                    }
                }
            }
        }
    }
    total += matcher.matches("istanbul.txt") ? 1U : 0U;
    total += static_cast<std::size_t>(matcher.score("ISTANBUL").value_or(0));
    total += se::match("x", se::Query{}).has_value() ? 1U : 0U;

    const se::RankInfo a{100, 1, "report.pdf", ""};
    const se::RankInfo b{100, 2, "report.pdf", "old"};
    total += se::ranks_before(a, b) ? 1U : 0U;
    return total;
}

// Types popup: summary, toggle, toggle all; persisted in the tab.
std::size_t use_types(const std::vector<fs::DirEntry>& entries, lay::TabState& tab) {
    std::vector<se::TypeCount> rows =
        se::summarize_types(Span<const fs::DirEntry>(entries), tab.show_hidden, tab.type_filter);
    std::size_t total = rows.size();
    if (!rows.empty()) {
        se::set_visible(tab.type_filter, rows.front().key, false);
        total += se::is_visible(tab.type_filter, rows.front().key) ? 0U : 1U;
    }
    se::toggle_all(tab.type_filter, Span<const se::TypeCount>(rows));
    total += tab.type_filter.active() ? 1U : 0U;
    for (const fs::DirEntry& entry : entries) {
        const se::TypeKey key = se::type_key_of(entry);
        total += key.kind == se::TypeKind::Folder ? 1U : key.extension.size();
        total += se::allows(tab.type_filter, entry) ? 1U : 0U;
    }
    const se::TypeKey folders{se::TypeKind::Folder, {}};
    total += folders < se::TypeKey{se::TypeKind::Extension, "jpg"} ? 1U : 0U;
    return total;
}

// An index is injectable: a fake IIndex for ui tests.
class FakeIndex final : public se::IIndex {
public:
    void start() override { status_.state = se::IndexState::Ready; }
    void stop() noexcept override { status_.state = se::IndexState::Stopped; }
    void request_rescan(se::RescanKind kind) override {
        status_.state =
            kind == se::RescanKind::Rebuild ? se::IndexState::Scanning : se::IndexState::Verifying;
    }
    [[nodiscard]] se::IndexStatus status() const override { return status_; }
    [[nodiscard]] const Path& root() const noexcept override { return root_; }
    [[nodiscard]] Result<se::IndexResult> query(const se::IndexQuery& query,
                                                std::stop_token stop) const override {
        if (stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "stopped");
        }
        if (!query.under.empty() && query.under.is_relative()) {
            return fail(ErrorCode::InvalidArgument, "relative");
        }
        se::IndexResult result;
        result.complete = status_.state == se::IndexState::Ready;
        result.generation = status_.generation;
        se::IndexHit hit;
        hit.path = root_ / "a.txt";
        hit.kind = fs::EntryKind::File;
        if (std::optional<se::Match> m = se::match("a.txt", query.query)) {
            hit.match = *m;
            result.hits.push_back(std::move(hit));
            result.matched = 1;
        }
        return result;
    }

private:
    Path root_ = "/";
    se::IndexStatus status_;
};

std::size_t use_index(const Path& cache_root, const std::vector<VolumeInfo>& volumes,
                      const WatcherFactory& watchers) {
    std::size_t total = 0;
    std::vector<std::unique_ptr<se::IIndex>> indexes;
    for (const VolumeInfo& volume : volumes) {
        if (!se::should_index_by_default(volume)) {
            continue;
        }
        se::NameIndexOptions options;
        options.root = volume.mount_point;
        options.identity = identity_of(volume);
        options.file = se::index_file(cache_root, options.identity);
        options.watcher_factory = watchers;
        options.watch_budget = se::kDefaultWatchBudget / 2;
        options.rescan_interval = std::chrono::minutes(30);
        options.on_status_change = [] {};
        total += se::index_stable_id(options.identity).size();
        if (Result<std::unique_ptr<se::IIndex>> index = se::create_name_index(std::move(options))) {
            indexes.push_back(std::move(*index));
        }
    }
    indexes.push_back(std::make_unique<FakeIndex>());
    for (const std::unique_ptr<se::IIndex>& index : indexes) {
        index->start();
        const se::IndexStatus status = index->status();
        total += static_cast<std::size_t>(status.entries + status.dirs_unwatched);
        total += status.watch_budget_exhausted ? 1U : 0U;
        total += static_cast<std::size_t>(status.load_duration.count());
        se::IndexQuery query;
        query.query = se::compile_query("budget", se::MatchMode::Fuzzy);
        query.under = index->root();
        query.limit = 100;
        const std::stop_source source;
        if (Result<se::IndexResult> result = index->query(query, source.get_token())) {
            for (const se::IndexHit& hit : result->hits) {
                total += hit.path.native().size() + hit.depth +
                         static_cast<std::size_t>(hit.match.score);
            }
            total += result->complete ? 1U : 0U;
        }
        index->request_rescan(se::RescanKind::Verify);
        index->stop();
    }
    return total;
}

// AppConfig gains the index switches; TabState the type filter (session + layouts).
std::size_t use_config(lay::TabState tab) {
    AppConfig config;
    config.cache_dir = "/tmp/navigator-cache";
    config.enable_index = false;
    tab.type_filter.hide_folders = true;
    const lay::TabState copy = tab;
    return (copy == tab ? 1U : 0U) + config.cache_dir.size();
}

// fold_name_into maps folded bytes back to raw bytes (highlights).
std::size_t use_fold() {
    std::string folded;
    std::vector<std::uint32_t> offsets;
    fs::fold_name_into("\xC4\xB0stanbul", folded, &offsets);
    return folded.size() + offsets.back();
}

} // namespace navigator::contracts_check
