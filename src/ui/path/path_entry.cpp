// src/ui/path/path_entry.cpp — see path_entry.hpp.
#include "path/path_entry.hpp"

#include <algorithm>
#include <cstdlib>
#include <set>

#include <navigator/i18n.hpp>
#include <navigator/platform.hpp>

#include "i18n/catalog.hpp"

namespace navigator::ui {
namespace {

constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

// Request ids are unique across every PathEntry (they share one worker).
std::uint64_t g_next_request = 1;

std::optional<std::string> env_lookup(std::string_view name) {
    const std::string key(name);
    // NOLINTNEXTLINE(concurrency-mt-unsafe): nothing in Navigator calls setenv after startup.
    const char* v = std::getenv(key.c_str());
    if (v == nullptr) {
        return std::nullopt;
    }
    return std::string(v);
}

bool has_prefix(const Path& p, const Path& base) {
    auto pi = p.begin();
    for (auto bi = base.begin(); bi != base.end(); ++bi, ++pi) {
        if (bi->empty()) {
            continue;
        }
        if (pi == p.end() || *pi != *bi) {
            return false;
        }
    }
    return true;
}

bool contains_folded(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) {
        return true;
    }
    return fs::fold_name(haystack).find(fs::fold_name(needle)) != std::string::npos;
}

PathItem::Kind item_kind(GotoSource::Kind k) {
    switch (k) {
    case GotoSource::Kind::Recent: return PathItem::Kind::Recent;
    case GotoSource::Kind::Bookmark: return PathItem::Kind::Bookmark;
    case GotoSource::Kind::Place: return PathItem::Kind::Place;
    }
    return PathItem::Kind::Place;
}

} // namespace

bool looks_like_path(std::string_view text) noexcept {
    std::size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) {
        ++i;
    }
    if (i >= text.size()) {
        return false;
    }
    const char c = text[i];
    if (c == '/' || c == '~' || c == '$' || c == '%' || c == '.' || c == '\'' || c == '"') {
        return true;
    }
    return text.find('/') != std::string_view::npos;
}

std::string display_path(const Path& path, const Path& home) {
    if (!home.empty() && home != home.root_path() && has_prefix(path, home)) {
        const Path rest = path.lexically_relative(home);
        if (rest.empty() || rest == ".") {
            return "~";
        }
        return fs::to_display_name("~/" + rest.string());
    }
    return fs::to_display_name(path.string());
}

path_input::ExpandContext PathEntry::ui_context() const {
    path_input::ExpandContext ctx;
    ctx.home = home_;
    ctx.cwd = cwd_;
    ctx.env = env_lookup;
    ctx.percent_variables = true;
    return ctx; // no user_home: "~name" resolves on the worker only
}

path_input::ExpandContext PathEntry::worker_context() const {
    path_input::ExpandContext ctx = ui_context();
    ctx.user_home = [](std::string_view name) -> std::optional<Path> {
        Result<Path> h = platform::user_home(name);
        if (!h) {
            return std::nullopt;
        }
        return *h;
    };
    return ctx;
}

void PathEntry::open(std::string_view text, bool select_all, const Path& cwd, const Path& home,
                     std::vector<GotoSource> sources, double now_ms) {
    open_ = true;
    edited_ = false;
    cwd_ = cwd;
    home_ = home;
    sources_ = std::move(sources);
    edit_.set(text, select_all);
    error_.clear();
    cache_ = Cache{};
    completions_.clear();
    debounce_at_.reset();
    completion_id_ = 0;
    resolve_id_ = 0;
    outcome_.reset();
    changed(now_ms);
    edited_ = false;
    highlight_ = mode_ == PathEntryMode::GoTo ? 0 : kNpos;
}

void PathEntry::close() {
    open_ = false;
    edited_ = false;
    items_.clear();
    completions_.clear();
    sources_.clear();
    error_.clear();
    debounce_at_.reset();
    resolve_id_ = 0;
    completion_id_ = 0;
    cache_ = Cache{};
}

void PathEntry::changed(double now_ms) {
    edited_ = true;
    error_.clear();
    const std::string& text = edit_.text();
    preview_.reset();
    if (!text.empty()) {
        if (Result<path_input::ExpandedPath> e = path_input::expand_user_path(text, ui_context());
            e) {
            preview_ = std::move(*e);
        }
    }
    completions_.clear();
    debounce_at_.reset();
    const bool want = mode_ == PathEntryMode::GoTo ? !text.empty() : true;
    if (want) {
        if (Result<path_input::CompletionQuery> q =
                path_input::parse_completion_input(text, ui_context());
            q) {
            wanted_key_ = q->typed_dir;
            if (cache_.valid && cache_.key == q->typed_dir) {
                path_input::CompletionQuery query = cache_.query;
                query.prefix = q->prefix;
                path_input::CompletionOptions o;
                o.max_results = kMaxCompletions;
                completions_ = path_input::match_completions(query, cache_.entries, o);
            } else {
                pending_input_ = text;
                debounce_at_ = now_ms + kDebounceMs;
            }
        }
    }
    rebuild();
    highlight_ = mode_ == PathEntryMode::GoTo && !items_.empty() ? 0 : kNpos;
}

void PathEntry::request_completion(double now_ms) {
    (void)now_ms;
    debounce_at_.reset();
    CompletionRequest req;
    req.id = g_next_request++;
    req.input = pending_input_;
    req.context = worker_context();
    completion_id_ = req.id;
    requested_key_ = wanted_key_;
    if (worker_ != nullptr) {
        worker_->complete(std::move(req));
    } else {
        apply_completion(run_completion(req));
    }
}

void PathEntry::tick(double now_ms) {
    if (open_ && debounce_at_ && now_ms >= *debounce_at_) {
        request_completion(now_ms);
    }
}

bool PathEntry::poll(double now_ms) {
    (void)now_ms;
    if (!open_ || worker_ == nullptr) {
        return false;
    }
    bool changed_any = false;
    if (std::optional<CompletionReply> r = worker_->take_completion(); r) {
        if (r->id == completion_id_) {
            apply_completion(std::move(*r));
            changed_any = true;
        }
    }
    if (std::optional<ResolveReply> r = worker_->take_resolve(); r) {
        if (r->id == resolve_id_) {
            apply_resolve(std::move(*r), resolve_new_tab_);
            changed_any = true;
        }
    }
    return changed_any;
}

void PathEntry::apply_completion(CompletionReply reply) {
    completion_id_ = 0;
    cache_.valid = reply.ok;
    cache_.key = requested_key_;
    cache_.query = std::move(reply.query);
    cache_.entries = std::move(reply.entries);
    // Re-match with the current text (the user may have typed on meanwhile).
    const std::size_t keep = highlight_;
    completions_.clear();
    if (Result<path_input::CompletionQuery> q =
            path_input::parse_completion_input(edit_.text(), ui_context());
        q && cache_.valid && q->typed_dir == cache_.key &&
        (mode_ == PathEntryMode::AddressBar || !edit_.text().empty())) {
        path_input::CompletionQuery query = cache_.query;
        query.prefix = q->prefix;
        path_input::CompletionOptions o;
        o.max_results = kMaxCompletions;
        completions_ = path_input::match_completions(query, cache_.entries, o);
    }
    rebuild();
    highlight_ = keep < items_.size()                              ? keep
                 : mode_ == PathEntryMode::GoTo && !items_.empty() ? 0
                                                                   : kNpos;
}

void PathEntry::rebuild() {
    items_.clear();
    const std::string& text = edit_.text();
    std::set<Path> seen;
    const auto add_completions = [&] {
        for (const path_input::Completion& c : completions_) {
            if (seen.contains(c.path)) {
                continue;
            }
            seen.insert(c.path);
            PathItem it;
            it.kind = PathItem::Kind::Completion;
            it.section = PathItem::Section::Matches;
            it.label = c.name;
            it.detail = display_path(c.path, home_);
            it.path = c.path;
            it.completion_text = c.text;
            it.is_symlink = c.is_symlink;
            items_.push_back(std::move(it));
        }
    };
    if (mode_ == PathEntryMode::AddressBar) {
        if (edited_) {
            add_completions();
        }
        return;
    }
    const auto add_source = [&](const GotoSource& s, PathItem::Section section) {
        if (seen.contains(s.path)) {
            return;
        }
        seen.insert(s.path);
        PathItem it;
        it.kind = item_kind(s.kind);
        it.section = section;
        it.label = s.label;
        it.detail = display_path(s.path, home_);
        it.path = s.path;
        it.status = s.status;
        it.place = s.place;
        items_.push_back(std::move(it));
    };
    const auto section_of = [](const GotoSource& s) {
        return s.kind == GotoSource::Kind::Recent ? PathItem::Section::Recents
                                                  : PathItem::Section::Places;
    };
    if (text.empty()) {
        for (const GotoSource& s : sources_) {
            if (s.kind == GotoSource::Kind::Recent) {
                add_source(s, PathItem::Section::Recents);
            }
        }
        for (const GotoSource& s : sources_) {
            if (s.kind != GotoSource::Kind::Recent) {
                add_source(s, PathItem::Section::Places);
            }
        }
        return;
    }
    // The typed text itself (Enter resolves it).
    {
        PathItem typed;
        typed.kind = PathItem::Kind::Typed;
        typed.section = PathItem::Section::Typed;
        typed.label = text;
        if (preview_) {
            typed.path = preview_->path;
            typed.detail = fs::to_display_name(preview_->path.string());
        }
        items_.push_back(std::move(typed));
    }
    if (looks_like_path(text)) {
        add_completions();
        const std::string needle =
            preview_ ? fs::to_display_name(preview_->path.string()) : std::string(text);
        for (const GotoSource& s : sources_) {
            if (s.kind != GotoSource::Kind::Place &&
                contains_folded(fs::to_display_name(s.path.string()), needle)) {
                add_source(s, section_of(s));
            }
        }
        return;
    }
    for (const GotoSource& s : sources_) {
        if (s.kind == GotoSource::Kind::Recent &&
            (contains_folded(s.label, text) ||
             contains_folded(fs::to_display_name(s.path.string()), text))) {
            add_source(s, PathItem::Section::Recents);
        }
    }
    for (const GotoSource& s : sources_) {
        if (s.kind != GotoSource::Kind::Recent &&
            (contains_folded(s.label, text) ||
             contains_folded(fs::to_display_name(s.path.string()), text))) {
            add_source(s, PathItem::Section::Places);
        }
    }
    add_completions();
}

std::optional<PathOutcome> PathEntry::take_outcome() {
    std::optional<PathOutcome> out;
    out.swap(outcome_);
    return out;
}

void PathEntry::move(std::ptrdiff_t delta) {
    if (items_.empty()) {
        highlight_ = kNpos;
        return;
    }
    const auto last = static_cast<std::ptrdiff_t>(items_.size()) - 1;
    if (highlight_ == kNpos) {
        if (delta > 0) {
            highlight_ = static_cast<std::size_t>(std::min(delta - 1, last));
        }
        return;
    }
    const std::ptrdiff_t next = static_cast<std::ptrdiff_t>(highlight_) + delta;
    if (mode_ == PathEntryMode::AddressBar && next < 0) {
        highlight_ = kNpos;
        return;
    }
    highlight_ = static_cast<std::size_t>(std::clamp(next, std::ptrdiff_t{0}, last));
}

void PathEntry::set_highlight(std::size_t index) noexcept {
    highlight_ = index < items_.size() ? index : kNpos;
}

bool PathEntry::accept_completion(double now_ms) {
    const PathItem* pick = nullptr;
    if (highlight_ < items_.size() && items_[highlight_].kind == PathItem::Kind::Completion) {
        pick = &items_[highlight_];
    } else {
        for (const PathItem& it : items_) {
            if (it.kind == PathItem::Kind::Completion) {
                pick = &it;
                break;
            }
        }
    }
    if (pick == nullptr) {
        return false;
    }
    const std::string text = pick->completion_text;
    edit_.set(text);
    changed(now_ms);
    return true;
}

void PathEntry::submit(bool new_tab) {
    if (resolve_id_ != 0) {
        return; // Enter already pressed; the reply decides
    }
    error_.clear();
    if (highlight_ < items_.size() && items_[highlight_].kind != PathItem::Kind::Typed) {
        const PathItem& it = items_[highlight_];
        if (it.status == places::LocationStatus::Offline) {
            error_ = substitute(tr("goto.offline"), "name", it.label);
            return;
        }
        if (it.status == places::LocationStatus::Missing) {
            error_ = substitute(tr("goto.not_found"), "path", display_path(it.path, home_));
            return;
        }
        PathOutcome o;
        o.kind = PathOutcome::Kind::Navigate;
        o.path = it.path;
        o.new_tab = new_tab;
        outcome_ = std::move(o);
        return;
    }
    if (edit_.text().empty()) {
        return;
    }
    ResolveRequest req;
    req.id = g_next_request++;
    req.input = edit_.text();
    req.context = worker_context();
    resolve_id_ = req.id;
    resolve_new_tab_ = new_tab;
    if (worker_ != nullptr) {
        worker_->resolve(std::move(req));
    } else {
        ResolveReply reply;
        reply.id = req.id;
        reply.result = path_input::resolve_input(req.input, req.context);
        apply_resolve(std::move(reply), new_tab);
    }
}

void PathEntry::apply_resolve(ResolveReply reply, bool new_tab) {
    resolve_id_ = 0;
    if (!reply.result) {
        error_ =
            reply.result.error().code == ErrorCode::InvalidArgument
                ? std::string(tr("goto.invalid"))
                : std::string(tr("error." + std::string(to_string(reply.result.error().code))));
        return;
    }
    const path_input::ResolvedInput& r = *reply.result;
    if (!r.exists()) {
        error_ = substitute(tr("goto.not_found"), "path", display_path(r.path, home_));
        if (!r.unresolved.empty()) {
            std::string names;
            for (const std::string& u : r.unresolved) {
                if (!names.empty()) {
                    names += ", ";
                }
                names += u;
            }
            error_ += "  ";
            error_ += substitute(tr("goto.unknown_variable"), "name", names);
        }
        return;
    }
    PathOutcome o;
    o.new_tab = new_tab;
    o.case_corrected = r.case_corrected;
    if (r.is_directory()) {
        o.kind = PathOutcome::Kind::Navigate;
        o.path = r.path;
    } else if (new_tab) {
        o.kind = PathOutcome::Kind::OpenFile;
        o.path = r.path;
    } else {
        o.kind = PathOutcome::Kind::RevealFile;
        o.path = r.path.parent_path();
        o.cursor_name = r.path.filename().string();
    }
    outcome_ = std::move(o);
}

bool PathEntry::busy() const noexcept {
    return debounce_at_.has_value() || completion_id_ != 0 || resolve_id_ != 0;
}

} // namespace navigator::ui
