// src/ui/browser/browser_model_search.cpp — Phase 5 parts of BrowserModel (see
// browser_model.hpp): rows, folder filter, type filter, flattened view, index-backed queries.
#include <algorithm>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "browser/browser_model.hpp"

namespace navigator::ui {
namespace {

constexpr std::uint32_t kNoId = 0xFFFFFFFFU;

// Type-only filtering does no matching, so it stays inline for bigger listings.
constexpr std::size_t kTypeOnlyFactor = 8;

// Identity of a flattened row across index results: (parent, name). '/' is in no name.
std::string row_identity(const FlatTables& tables, std::uint32_t id) {
    std::string key(tables.parent_of(id));
    key += '/';
    key += tables.entry(id).entry.name;
    return key;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------------------------

std::size_t BrowserModel::row_count() const noexcept {
    if (flat_) {
        return flat_->rows.size();
    }
    return list_identity_ ? entries_.size() : list_rows_.size();
}

const fs::DirEntry& BrowserModel::row_entry(std::size_t row) const noexcept {
    if (flat_) {
        return flat_->tables.entry(flat_->rows[row]).entry;
    }
    return entries_[list_src(row)];
}

std::string_view BrowserModel::row_parent(std::size_t row) const noexcept {
    if (!flat_ || row >= flat_->rows.size()) {
        return {};
    }
    return flat_->tables.parent_of(flat_->rows[row]);
}

std::uint32_t BrowserModel::row_depth(std::size_t row) const noexcept {
    if (!flat_ || row >= flat_->rows.size()) {
        return 1;
    }
    return flat_->tables.entry(flat_->rows[row]).depth;
}

Path BrowserModel::row_path(std::size_t row) const {
    if (row >= row_count()) {
        return {};
    }
    if (!flat_) {
        return current_.dir / row_entry(row).name;
    }
    const std::string_view parent = row_parent(row);
    Path p = flat_->request.root;
    if (!parent.empty()) {
        p /= Path(std::string(parent));
    }
    p /= row_entry(row).name;
    return p;
}

std::size_t BrowserModel::source_count() const noexcept {
    if (flat_) {
        return flat_->request.index ? static_cast<std::size_t>(flat_->matched)
                                    : flat_->tables.size();
    }
    return entries_.size();
}

std::size_t BrowserModel::find_row(std::string_view name) const noexcept {
    if (flat_) {
        for (std::size_t i = 0; i < flat_->rows.size(); ++i) {
            if (flat_->tables.entry(flat_->rows[i]).entry.name == name) {
                return i;
            }
        }
        return kNoRow;
    }
    const std::size_t src = find(name);
    return src == kNoRow ? kNoRow : list_row_of(src);
}

std::size_t BrowserModel::list_src(std::size_t row) const noexcept {
    if (list_identity_) {
        return row < entries_.size() ? row : kNoRow;
    }
    return row < list_rows_.size() ? list_rows_[row] : kNoRow;
}

std::size_t BrowserModel::list_row_of(std::size_t src) const noexcept {
    if (src == kNoRow) {
        return kNoRow;
    }
    if (list_identity_) {
        return src < entries_.size() ? src : kNoRow;
    }
    const auto it =
        std::lower_bound(list_rows_.begin(), list_rows_.end(), static_cast<std::uint32_t>(src));
    if (it == list_rows_.end() || *it != src) {
        return kNoRow;
    }
    return static_cast<std::size_t>(it - list_rows_.begin());
}

std::size_t BrowserModel::flat_row_of(std::uint32_t id) const noexcept {
    if (!flat_ || id == kNoId) {
        return kNoRow;
    }
    for (std::size_t i = 0; i < flat_->rows.size(); ++i) {
        if (flat_->rows[i] == id) {
            return i;
        }
    }
    return kNoRow;
}

bool BrowserModel::listing_filtering() const noexcept { return !query_.empty() || types_.active(); }

bool BrowserModel::entry_shown(const fs::DirEntry& e) {
    return search::allows(types_, e) && matcher_.matches(e.name);
}

bool BrowserModel::rows_pending() const noexcept {
    if (flat_) {
        return flat_->rows.empty() && (flat_->in_flight || flat_->query_due.has_value());
    }
    return !list_identity_ && list_rows_version_ != listing_version_;
}

const search::Query& BrowserModel::query() const noexcept { return flat_ ? flat_->query : query_; }

search::MatchMode BrowserModel::match_mode() const noexcept {
    return flat_ ? flat_->mode : list_mode_;
}

bool BrowserModel::header_sorted() const noexcept { return flat_ && flat_->header_sort; }

bool BrowserModel::flatten_is_volume() const noexcept { return flat_ && flat_->request.volume; }

const Path& BrowserModel::flatten_root() const noexcept {
    static const Path kEmpty;
    return flat_ ? flat_->request.root : kEmpty;
}

// ---------------------------------------------------------------------------------------------
// Filter text and type filter
// ---------------------------------------------------------------------------------------------

void BrowserModel::compile_query() {
    query_ = search::compile_query(filter_.text(), list_mode_);
    matcher_ = search::Matcher(query_);
    if (flat_) {
        flat_->query = search::compile_query(filter_.text(), flat_->mode);
    }
}

void BrowserModel::filter_edited() {
    if (filter_.text() == (flat_ ? flat_->query.text : query_.text)) {
        return;
    }
    query_changed(true);
}

void BrowserModel::set_filter_text(std::string_view text) {
    filter_.set(text);
    filter_.end(false);
    query_changed(true);
}

void BrowserModel::clear_filter() {
    if (filter_.empty()) {
        return;
    }
    filter_.clear();
    query_changed(true);
}

void BrowserModel::toggle_match_mode() {
    if (flat_) {
        flat_->mode = flat_->mode == search::MatchMode::Fuzzy ? search::MatchMode::Substring
                                                              : search::MatchMode::Fuzzy;
    } else {
        list_mode_ = list_mode_ == search::MatchMode::Fuzzy ? search::MatchMode::Substring
                                                            : search::MatchMode::Fuzzy;
    }
    query_changed(true);
}

void BrowserModel::set_type_filter(search::TypeFilter filter) {
    search::normalize(filter);
    if (filter == types_) {
        return;
    }
    types_ = std::move(filter);
    type_rows_valid_ = false;
    query_changed(false);
}

const std::vector<search::TypeCount>& BrowserModel::type_summary() const {
    if (flat_) {
        return flat_->types;
    }
    const std::uint64_t key = (listing_version_ * 1000003ULL) ^ filter_gen_;
    if (!type_rows_valid_ || type_rows_key_ != key) {
        type_rows_ =
            search::summarize_types(Span<const fs::DirEntry>(entries_), show_hidden_, types_);
        type_rows_key_ = key;
        type_rows_valid_ = true;
    }
    return type_rows_;
}

void BrowserModel::query_changed(bool text) {
    ++filter_gen_;
    compile_query();
    if (flat_) {
        if (text) {
            flat_->header_sort = false; // a new query goes back to relevance order
        }
        flat_->cursor_follows_top = true;
        flat_->reveal = true;
        flat_->refreshing = false;
        if (flat_->request.index) {
            flat_->query_due = std::chrono::steady_clock::now() + options_.index_debounce;
            if (options_.index_debounce.count() <= 0) {
                post_index_query();
            }
        } else {
            post_flat_job(true);
        }
        list_stale_ = true; // the listing under the flattened view catches up on leave
        ++content_serial_;
        return;
    }
    // A new query puts the cursor on the first match; clearing the filter keeps the cursor on
    // the entry it was on.
    const std::size_t cs = list_src(cursor_);
    const std::size_t as = list_src(anchor_);
    rebuild_listing_rows(listing_filtering() ? CursorPolicy::Top : CursorPolicy::Keep, cs, as);
    ++content_serial_;
}

std::shared_ptr<const std::vector<fs::DirEntry>> BrowserModel::listing_snapshot() {
    if (!snapshot_ || snapshot_version_ != listing_version_) {
        snapshot_ = std::make_shared<const std::vector<fs::DirEntry>>(entries_);
        snapshot_version_ = listing_version_;
    }
    return snapshot_;
}

void BrowserModel::rebuild_listing_rows(CursorPolicy policy, std::size_t cursor_src,
                                        std::size_t anchor_src) {
    // Rows that went stale under a flattened view skipped watch inserts: never narrow from them.
    const bool stale = list_stale_;
    list_stale_ = false;
    if (!listing_filtering()) {
        // Nothing filters: the rows are the listing again.
        list_identity_ = true;
        list_rows_.clear();
        if (listing_job_out_) {
            ++listing_job_; // drop the outstanding result
            listing_job_out_ = false;
        }
        if (listing_shown()) {
            const std::size_t n = entries_.size();
            if (policy == CursorPolicy::Keep && cursor_src < n) {
                cursor_ = cursor_src;
            } else {
                cursor_ = n == 0 ? kNoRow : 0;
            }
            anchor_ = (policy == CursorPolicy::Keep && anchor_src < n) ? anchor_src : cursor_;
            band_active_ = false;
            recount();
            ++cursor_serial_;
        }
        return;
    }
    const bool had_rows = !stale && !list_identity_ && list_rows_version_ == listing_version_;
    const bool narrow = had_rows && !listing_job_out_ && list_rows_types_ == types_ &&
                        search::narrows(list_rows_query_, query_);
    const std::size_t candidates = narrow ? list_rows_.size() : entries_.size();
    const std::size_t limit = options_.ui_filter_limit;
    const bool inline_ok = suspended_ || candidates <= limit ||
                           (query_.empty() && candidates <= limit * kTypeOnlyFactor);
    if (inline_ok) {
        std::vector<std::uint32_t> rows;
        const std::vector<std::uint32_t> subset =
            narrow ? list_rows_ : std::vector<std::uint32_t>{};
        if (filter_listing_rows(entries_, matcher_, types_, narrow ? &subset : nullptr, rows)) {
            if (listing_job_out_) {
                ++listing_job_;
                listing_job_out_ = false;
            }
            list_identity_ = false;
            set_listing_rows(std::move(rows), policy, cursor_src, anchor_src);
        }
        return;
    }
    // Worker: the old rows stay shown (when still valid) until the newest result arrives.
    if (list_identity_) {
        // From identity: keep showing everything until the first result (no blank flash).
        list_rows_.resize(entries_.size());
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            list_rows_[i] = static_cast<std::uint32_t>(i);
        }
        list_rows_version_ = listing_version_;
        list_rows_query_ = search::Query{};
        list_rows_types_ = search::TypeFilter{};
        list_identity_ = false;
    }
    const std::uint64_t job = ++listing_job_;
    listing_job_out_ = true;
    pending_top_ = policy == CursorPolicy::Top;
    pending_cursor_name_ = cursor_src < entries_.size() ? entries_[cursor_src].name : std::string();
    pending_anchor_name_ = anchor_src < entries_.size() ? entries_[anchor_src].name : std::string();
    std::optional<std::vector<std::uint32_t>> subset;
    if (narrow) {
        subset = list_rows_;
    }
    worker().post(
        SearchWorker::kListingLane,
        [box = listing_box_, snap = listing_snapshot(), q = query_, types = types_,
         subset = std::move(subset), job, version = listing_version_,
         wake = options_.wake](const std::stop_token& stop) {
            search::Matcher m(q);
            std::vector<std::uint32_t> out;
            const Result<void> r =
                filter_listing_rows(*snap, m, types, subset ? &*subset : nullptr, out, stop);
            if (!r && r.error().code == ErrorCode::Cancelled) {
                return;
            }
            ListingFilterResult res;
            res.job = job;
            res.version = version;
            if (r) {
                res.rows = std::move(out);
            } else {
                res.rows = fail(r.error().code, r.error().message);
            }
            box->push(std::move(res));
            if (wake) {
                wake();
            }
        },
        true);
}

void BrowserModel::set_listing_rows(std::vector<std::uint32_t> rows, CursorPolicy policy,
                                    std::size_t cursor_src, std::size_t anchor_src) {
    list_rows_ = std::move(rows);
    list_rows_version_ = listing_version_;
    list_rows_query_ = query_;
    list_rows_types_ = types_;
    list_identity_ = false;
    // Entries the filter hides are never selected (selection = shown rows only).
    if (std::any_of(selected_.begin(), selected_.end(), [](std::uint8_t f) { return f != 0; })) {
        std::vector<std::uint8_t> keep(selected_.size(), 0);
        for (const std::uint32_t i : list_rows_) {
            if (i < keep.size()) {
                keep[i] = selected_[i];
            }
        }
        selected_.swap(keep);
    }
    if (!listing_shown()) {
        return;
    }
    const std::size_t n = list_rows_.size();
    if (policy == CursorPolicy::Top) {
        cursor_ = n == 0 ? kNoRow : 0;
        anchor_ = cursor_;
    } else {
        cursor_ = list_row_of(cursor_src);
        if (cursor_ == kNoRow && n > 0) {
            cursor_ = 0;
        }
        anchor_ = list_row_of(anchor_src);
        if (anchor_ == kNoRow) {
            anchor_ = cursor_;
        }
    }
    band_active_ = false;
    recount();
    ++cursor_serial_;
    ++content_serial_;
}

void BrowserModel::listing_inserted(std::size_t src) {
    const auto at = static_cast<std::uint32_t>(src);
    auto it = std::lower_bound(list_rows_.begin(), list_rows_.end(), at);
    for (auto j = it; j != list_rows_.end(); ++j) {
        ++*j;
    }
    const bool valid = list_rows_version_ + 1 == listing_version_;
    if (valid) {
        list_rows_version_ = listing_version_;
    }
    if (list_stale_ || !valid || !entry_shown(entries_[src])) {
        return;
    }
    const auto k = static_cast<std::size_t>(it - list_rows_.begin());
    list_rows_.insert(it, at);
    if (listing_shown()) {
        if (cursor_ != kNoRow && cursor_ >= k) {
            ++cursor_;
        }
        if (anchor_ != kNoRow && anchor_ >= k) {
            ++anchor_;
        }
    }
}

void BrowserModel::listing_erased(std::size_t src) {
    const auto at = static_cast<std::uint32_t>(src);
    auto it = std::lower_bound(list_rows_.begin(), list_rows_.end(), at);
    const auto k = static_cast<std::size_t>(it - list_rows_.begin());
    const bool had = it != list_rows_.end() && *it == at;
    if (had) {
        it = list_rows_.erase(it);
    }
    for (auto j = it; j != list_rows_.end(); ++j) {
        --*j;
    }
    if (list_rows_version_ + 1 == listing_version_) {
        list_rows_version_ = listing_version_;
    }
    if (!had || !listing_shown()) {
        return;
    }
    const std::size_t n = list_rows_.size();
    const auto fix = [&](std::size_t& index) {
        if (index == kNoRow) {
            return;
        }
        if (index > k) {
            --index;
        } else if (index == k && index >= n) {
            index = n == 0 ? kNoRow : n - 1;
        }
    };
    fix(cursor_);
    fix(anchor_);
}

void BrowserModel::reset_search_view() {
    if (flat_) {
        if (!filter_.empty()) {
            // Cleared first, so leaving restores the listing cursor on the unfiltered rows.
            filter_.clear();
            compile_query();
            list_stale_ = true;
        }
        leave_flatten();
        return;
    }
    if (!filter_.empty()) {
        filter_.clear();
        ++filter_gen_;
        compile_query();
        rebuild_listing_rows(CursorPolicy::Keep, list_src(cursor_), list_src(anchor_));
        ++content_serial_;
    }
}

SearchWorker& BrowserModel::worker() {
    if (!worker_) {
        worker_ = std::make_unique<SearchWorker>();
    }
    return *worker_;
}

// ---------------------------------------------------------------------------------------------
// Flatten
// ---------------------------------------------------------------------------------------------

void BrowserModel::flatten(FlattenRequest request) {
    if (!loaded_ || suspended_) {
        return;
    }
    if (request.root.empty()) {
        request.root = current_.dir;
    }
    std::string list_cursor;
    float list_scroll = scroll_;
    if (flat_) {
        list_cursor = flat_->list_cursor_name;
        list_scroll = flat_->list_scroll;
        if (flat_->walk) {
            flat_->walk->cancel();
        }
        if (worker_) {
            worker_->cancel(SearchWorker::kFlatLane);
        }
    } else if (cursor_ < row_count()) {
        list_cursor = row_entry(cursor_).name;
    }
    flat_ = std::make_unique<Flat>();
    flat_->request = std::move(request);
    flat_->list_cursor_name = std::move(list_cursor);
    flat_->list_scroll = list_scroll;
    compile_query();
    ++filter_gen_;
    cursor_ = kNoRow;
    anchor_ = kNoRow;
    band_active_ = false;
    band_base_.clear();
    if (flat_->request.index) {
        post_index_query();
    } else {
        start_walk();
    }
    restored_scroll_ = 0.0F;
    scroll_ = 0.0F;
    recount();
    ++location_serial_;
    ++content_serial_;
    ++cursor_serial_;
}

void BrowserModel::leave_flatten() {
    if (!flat_) {
        return;
    }
    if (flat_->walk) {
        flat_->walk->cancel();
    }
    if (worker_) {
        worker_->cancel(SearchWorker::kFlatLane);
    }
    const std::string name = std::move(flat_->list_cursor_name);
    const float scroll = flat_->list_scroll;
    flat_.reset();
    ++filter_gen_;
    const std::size_t src = name.empty() ? kNoRow : find(name);
    if (list_stale_ || (!list_identity_ && list_rows_version_ != listing_version_)) {
        cursor_ = kNoRow;
        anchor_ = kNoRow;
        rebuild_listing_rows(CursorPolicy::Keep, src, src);
    } else {
        cursor_ = list_row_of(src);
        if (cursor_ == kNoRow && row_count() > 0) {
            cursor_ = 0;
        }
        anchor_ = cursor_;
    }
    band_active_ = false;
    band_base_.clear();
    restored_scroll_ = scroll;
    scroll_ = scroll;
    recount();
    ++location_serial_;
    ++content_serial_;
    ++cursor_serial_;
}

void BrowserModel::start_walk() {
    if (!flat_) {
        return;
    }
    Flat& f = *flat_;
    if (f.walk) {
        f.walk->cancel();
        f.walk.reset();
    }
    f.tables.clear();
    f.sel.clear();
    f.rows.clear();
    f.types.clear();
    f.stats = fs::WalkStats{};
    f.error.reset();
    f.cancelled = false;
    f.walk_done = false;
    f.interrupted = false;
    f.cursor_follows_top = true;
    cursor_ = kNoRow;
    anchor_ = kNoRow;
    recount();
    ++f.config;
    f.state = std::make_shared<OrganizeState>();
    f.sent_chunks = 0;
    f.in_flight = false;
    if (worker_) {
        worker_->cancel(SearchWorker::kFlatLane);
    }
    if (suspended_) {
        f.walking = false;
        return; // resume() walks
    }
    fs::WalkOptions o;
    o.root = f.request.root;
    o.show_hidden = show_hidden_;
    o.metadata = true;
    o.follow_symlinks = false;
    o.cross_filesystems = false;
    o.skip_unreadable = true;
    f.walk = std::make_unique<FlattenWalk>(std::move(o), options_.walk_factory, options_.wake);
    f.walking = true;
    ++content_serial_;
}

void BrowserModel::cancel_search() {
    if (!flat_) {
        return;
    }
    Flat& f = *flat_;
    if (f.walk) {
        f.walk->cancel();
        // Keep everything delivered so far: those rows stay (docs/keymap.md).
        FlattenWalk::Update u = f.walk->take();
        for (const FlatChunkPtr& c : u.chunks) {
            f.tables.append(c);
        }
        f.sel.resize(f.tables.size(), 0);
        f.stats = u.stats;
        f.walk.reset(); // detaches; never joined here
        f.walking = false;
        f.cancelled = true;
        if (!f.in_flight && f.sent_chunks < f.tables.chunk_count()) {
            post_flat_job(false);
        }
    }
    if (f.request.index && (f.in_flight || f.query_due)) {
        f.query_due.reset();
        if (worker_) {
            worker_->cancel(SearchWorker::kFlatLane);
        }
        f.in_flight = false;
        f.cancelled = true;
    }
    ++content_serial_;
}

void BrowserModel::post_flat_job(bool reset) {
    if (!flat_ || suspended_) {
        return;
    }
    Flat& f = *flat_;
    if (f.request.index) {
        post_index_query();
        return;
    }
    if (reset) {
        ++f.config;
        f.state = std::make_shared<OrganizeState>();
        f.sent_chunks = 0;
    }
    const std::vector<FlatChunkPtr>& all = f.tables.chunks();
    std::vector<FlatChunkPtr> chunks(all.begin() + static_cast<std::ptrdiff_t>(f.sent_chunks),
                                     all.end());
    if (chunks.empty() && !reset) {
        return;
    }
    f.sent_chunks = all.size();
    OrganizeConfig cfg;
    cfg.query = f.query;
    cfg.types = types_;
    cfg.sort = sort_;
    cfg.rank = !f.query.empty() && !f.header_sort;
    f.in_flight = true;
    worker().post(
        SearchWorker::kFlatLane,
        [box = flat_box_, state = f.state, cfg = std::move(cfg), chunks = std::move(chunks),
         config = f.config, wake = options_.wake](const std::stop_token& stop) {
            Result<OrganizeOutput> out = organize(*state, cfg, chunks, stop);
            if (!out && out.error().code == ErrorCode::Cancelled) {
                return;
            }
            FlatResult r;
            r.config = config;
            r.out = std::move(out);
            box->push(std::move(r));
            if (wake) {
                wake();
            }
        },
        reset);
}

void BrowserModel::post_index_query() {
    if (!flat_ || !flat_->request.index || suspended_) {
        return;
    }
    Flat& f = *flat_;
    f.query_due.reset();
    ++f.config;
    f.in_flight = true;
    f.refreshing = false;
    f.cancelled = false;
    f.status_checked = std::chrono::steady_clock::now();
    search::IndexQuery q;
    q.query = f.query;
    q.under = f.request.root;
    q.types = types_;
    q.show_hidden = show_hidden_;
    q.limit = options_.index_limit;
    OrganizeConfig cfg;
    cfg.query = f.query;
    cfg.types = types_;
    cfg.sort = sort_;
    cfg.rank = !f.query.empty() && !f.header_sort;
    worker().post(
        SearchWorker::kFlatLane,
        [box = flat_box_, index = f.request.index, q = std::move(q), cfg = std::move(cfg),
         config = f.config, root = f.request.root,
         wake = options_.wake](const std::stop_token& stop) {
            const auto post = [&](FlatResult r) {
                box->push(std::move(r));
                if (wake) {
                    wake();
                }
            };
            Result<search::IndexResult> res = index->query(q, stop);
            FlatResult r;
            r.config = config;
            if (!res) {
                if (res.error().code == ErrorCode::Cancelled) {
                    return;
                }
                r.out = fail(res.error().code, res.error().message);
                post(std::move(r));
                return;
            }
            // Rows carry the index's names and kinds; Size / Modified come from one stat per
            // hit (a few hundred on a local volume, on this worker).
            auto chunk = std::make_shared<FlatChunk>();
            chunk->first_dir = 1;
            std::unordered_map<std::string, std::uint32_t> dir_ids;
            chunk->entries.reserve(res->hits.size());
            std::size_t n = 0;
            for (const search::IndexHit& hit : res->hits) {
                if (++n % 64 == 0 && stop.stop_requested()) {
                    return;
                }
                fs::WalkEntry we;
                we.depth = hit.depth;
                const Path rel = hit.path.lexically_relative(root);
                const std::string parent = rel.has_parent_path() ? rel.parent_path().string() : "";
                if (!parent.empty() && parent != ".") {
                    auto [it, inserted] = dir_ids.try_emplace(
                        parent, static_cast<std::uint32_t>(chunk->new_dirs.size() + 1));
                    if (inserted) {
                        chunk->new_dirs.push_back(parent);
                    }
                    we.dir = it->second;
                }
                if (Result<fs::DirEntry> st = fs::stat(hit.path); st) {
                    we.entry = std::move(*st);
                } else {
                    we.entry.kind = hit.kind;
                    we.entry.metadata_error = st.error().code;
                }
                we.entry.name = hit.path.filename().string();
                we.entry.hidden = fs::is_hidden_name(we.entry.name);
                if (hit.directory_like && we.entry.kind == fs::EntryKind::Symlink) {
                    we.entry.target_kind = fs::EntryKind::Directory;
                }
                chunk->entries.push_back(std::move(we));
            }
            OrganizeState state;
            const FlatChunkPtr shared = chunk;
            Result<OrganizeOutput> out =
                organize(state, cfg, Span<const FlatChunkPtr>(&shared, 1), stop);
            if (!out && out.error().code == ErrorCode::Cancelled) {
                return;
            }
            if (out) {
                // Index results only contain allowed types: offer the hidden ones back.
                out->types = type_rows(state.counts, cfg.types, true);
            }
            r.out = std::move(out);
            r.chunk = shared;
            r.matched = res->matched;
            r.complete = res->complete;
            r.index_generation = res->generation;
            post(std::move(r));
        },
        true);
}

void BrowserModel::cancel_for_quit() noexcept {
    if (flat_ && flat_->walk) {
        flat_->walk->cancel();
    }
}

void BrowserModel::place_flat_cursor(std::uint32_t cursor_id, std::uint32_t anchor_id) {
    Flat& f = *flat_;
    const std::size_t n = f.rows.size();
    if (n == 0) {
        cursor_ = kNoRow;
        anchor_ = kNoRow;
        return;
    }
    if (f.cursor_follows_top) {
        cursor_ = 0;
        anchor_ = 0;
        return;
    }
    cursor_ = flat_row_of(cursor_id);
    if (cursor_ == kNoRow) {
        cursor_ = 0;
        f.cursor_follows_top = true;
    }
    anchor_ = flat_row_of(anchor_id);
    if (anchor_ == kNoRow) {
        anchor_ = cursor_;
    }
}

void BrowserModel::apply_flat_result(FlatResult& r) {
    Flat& f = *flat_;
    if (r.config != f.config) {
        return; // a superseded query or sort
    }
    f.in_flight = false;
    f.refreshing = false;
    if (!r.out) {
        f.error = r.out.error();
        ++content_serial_;
        return;
    }
    std::uint32_t cursor_id = cursor_ < f.rows.size() ? f.rows[cursor_] : kNoId;
    std::uint32_t anchor_id = anchor_ < f.rows.size() ? f.rows[anchor_] : kNoId;
    if (r.chunk) {
        // Index source: each answer replaces the rows' data. A live index answers again every
        // time the volume changes, so selection, cursor and anchor follow (parent, name).
        const std::string cursor_key =
            cursor_id == kNoId ? std::string() : row_identity(f.tables, cursor_id);
        const std::string anchor_key =
            anchor_id == kNoId ? std::string() : row_identity(f.tables, anchor_id);
        std::unordered_set<std::string> selected;
        if (selected_count_ > 0) {
            for (const std::uint32_t id : f.rows) {
                if (f.sel[id] != 0) {
                    selected.insert(row_identity(f.tables, id));
                }
            }
        }
        f.tables.clear();
        f.tables.append(r.chunk);
        f.sel.assign(f.tables.size(), 0);
        cursor_id = kNoId;
        anchor_id = kNoId;
        for (const std::uint32_t id : r.out->order) {
            const std::string key = row_identity(f.tables, id);
            f.sel[id] = selected.contains(key) ? 1 : 0;
            if (key == cursor_key) {
                cursor_id = id;
            }
            if (key == anchor_key) {
                anchor_id = id;
            }
        }
        f.matched = r.matched;
        f.index_complete = r.complete;
        f.index_generation = r.index_generation;
        f.error.reset();
    }
    f.rows = std::move(r.out->order);
    f.types = std::move(r.out->types);
    // Selection = shown rows only.
    if (selected_count_ > 0 && !r.chunk) {
        std::vector<std::uint8_t> keep(f.sel.size(), 0);
        for (const std::uint32_t id : f.rows) {
            keep[id] = f.sel[id];
        }
        f.sel.swap(keep);
    }
    place_flat_cursor(cursor_id, anchor_id);
    band_active_ = false;
    if (selected_count_ > 0) {
        recount(); // O(rows): only when there is a selection to count
    }
    if (f.reveal) {
        f.reveal = false;
        ++cursor_serial_;
    }
    ++content_serial_;
    if (!r.chunk && f.sent_chunks < f.tables.chunk_count()) {
        post_flat_job(false); // more arrived meanwhile
    }
}

bool BrowserModel::apply_search_results() {
    bool changed = false;
    for (ListingFilterResult& r : listing_box_->take()) {
        if (r.job != listing_job_) {
            continue;
        }
        listing_job_out_ = false;
        const std::size_t cs = pending_cursor_name_.empty() ? kNoRow : find(pending_cursor_name_);
        const std::size_t as = pending_anchor_name_.empty() ? kNoRow : find(pending_anchor_name_);
        const CursorPolicy policy = pending_top_ ? CursorPolicy::Top : CursorPolicy::Keep;
        if (r.version != listing_version_) {
            rebuild_listing_rows(policy, cs, as); // the listing changed meanwhile: again
            continue;
        }
        if (!r.rows) {
            continue;
        }
        set_listing_rows(std::move(*r.rows), policy, cs, as);
        changed = true;
    }
    if (!flat_) {
        (void)flat_box_->take();
        return changed;
    }
    Flat& f = *flat_;
    if (f.walk) {
        FlattenWalk::Update u = f.walk->take();
        if (!u.chunks.empty()) {
            for (const FlatChunkPtr& c : u.chunks) {
                f.tables.append(c);
            }
            f.sel.resize(f.tables.size(), 0);
            changed = true;
        }
        const bool progress = u.stats.entries_returned != f.stats.entries_returned ||
                              u.stats.dirs_visited != f.stats.dirs_visited;
        f.stats = u.stats;
        changed = changed || progress;
        if (u.finished) {
            f.walking = false;
            f.walk_done = !u.cancelled;
            f.cancelled = u.cancelled;
            f.error = u.error;
            f.walk.reset();
            changed = true;
        }
        if (!f.in_flight && f.sent_chunks < f.tables.chunk_count()) {
            post_flat_job(false);
        } else if (!f.in_flight && f.walk_done && f.tables.size() == 0 && f.rows.empty()) {
            ++content_serial_; // an empty subtree: the empty state shows
        }
    }
    for (FlatResult& r : flat_box_->take()) {
        if (!flat_) {
            break;
        }
        apply_flat_result(r);
        changed = true;
    }
    if (flat_ && flat_->request.index) {
        const auto now = std::chrono::steady_clock::now();
        const auto due = flat_->query_due;
        if (due.has_value() && now >= *due) {
            post_index_query();
        } else if (!flat_->in_flight && !flat_->query_due && !flat_->cancelled &&
                   now - flat_->status_checked >= std::chrono::seconds(1)) {
            // The index moved on (scan progress, live updates): re-run the visible query.
            flat_->status_checked = now;
            if (flat_->request.index->status().generation != flat_->index_generation) {
                post_index_query();
                flat_->refreshing = true;
            }
        }
    }
    return changed;
}

FlattenProgress BrowserModel::flatten_progress() const {
    FlattenProgress p;
    if (!flat_) {
        return p;
    }
    const Flat& f = *flat_;
    p.root = f.request.root;
    p.volume = f.request.volume;
    p.from_index = f.request.index != nullptr;
    p.walking = f.walking;
    // A live index re-answering the same query is not a search the user waits for.
    p.organizing = !f.refreshing && (f.in_flight || f.query_due.has_value());
    p.cancelled = f.cancelled;
    p.truncated = f.stats.truncated;
    p.error = f.error;
    p.stats = f.stats;
    p.found = p.from_index ? f.matched : f.tables.size();
    p.skipped = f.stats.skipped_permission + f.stats.skipped_vanished + f.stats.skipped_errors;
    p.index_complete = f.index_complete;
    p.limited = p.from_index && f.matched > f.tables.size();
    return p;
}

bool BrowserModel::searching() const noexcept {
    if (listing_job_out_) {
        return true;
    }
    return flat_ && (flat_->walking || flat_->in_flight || flat_->query_due.has_value());
}

bool BrowserModel::settle_search(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        (void)frame_start();
        if (!searching()) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

} // namespace navigator::ui
