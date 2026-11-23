// src/ops/context.cpp: see context.hpp.
#include "context.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace navigator::ops::detail {
namespace {

constexpr std::chrono::milliseconds kRateWindow{5000};
constexpr std::chrono::milliseconds kRateWarmup{2000};
constexpr std::chrono::milliseconds kSampleEvery{250};

[[nodiscard]] Path record_volume(const Path& path, const ProtectedPaths& pp) {
    Path best = "/";
    std::size_t best_len = 1;
    for (const Path& mp : pp.mount_points) {
        if (!mp.empty() && is_same_or_inside(path, mp) && mp.native().size() > best_len) {
            best = mp;
            best_len = mp.native().size();
        }
    }
    return best;
}

void push_unique(std::vector<Path>& v, const Path& p) {
    if (p.empty()) {
        return;
    }
    if (std::find(v.begin(), v.end(), p) == v.end()) {
        v.push_back(p);
    }
}

} // namespace

Context::Context(const OpRequest& request, const EngineOptions& options,
                 const ConflictResolver& resolve, const ProgressSink& sink, std::stop_token stop,
                 const RunHooks* hooks)
    : request_(request), options_(options), resolve_(resolve), sink_(sink), stop_(std::move(stop)),
      hooks_(hooks), start_(Clock::now()) {
    progress_.id = hooks_ != nullptr ? hooks_->op : OpId{};
    progress_.kind = request.kind;
    progress_.origin = request.origin;
    progress_.phase = OpPhase::Scanning;
    progress_.destination = request.destination;
    result_.id = progress_.id;
    result_.kind = request.kind;
    result_.origin = request.origin;
}

std::size_t Context::chunk() const noexcept {
    return options_.copy_chunk_bytes == 0 ? std::size_t{1} << 20U : options_.copy_chunk_bytes;
}

bool Context::cancelled() const noexcept { return internal_cancel_ || stop_.stop_requested(); }

bool Context::checkpoint() {
    if (cancelled()) {
        return false;
    }
    if (hooks_ != nullptr && hooks_->pause_requested && hooks_->pause_requested()) {
        const OpPhase before = progress_.phase;
        progress_.phase = OpPhase::Paused;
        emit(true);
        const auto t0 = Clock::now();
        if (hooks_->wait_while_paused) {
            hooks_->wait_while_paused();
        }
        excluded_ += Clock::now() - t0;
        samples_.clear(); // the rate restarts after a pause
        progress_.phase = before;
        emit(true);
    }
    return !cancelled();
}

void Context::set_phase(OpPhase phase) {
    if (progress_.phase != phase) {
        progress_.phase = phase;
        emit(true);
    }
}

void Context::add_total(std::uint64_t items, std::uint64_t bytes) {
    progress_.items_total += items;
    progress_.bytes_total += bytes;
}

void Context::set_totals_final() {
    progress_.totals_final = true;
    emit(true);
}

void Context::add_bytes(std::uint64_t bytes) {
    progress_.bytes_done += bytes;
    result_.bytes_done += bytes;
    emit(false);
}

void Context::add_items(std::uint64_t items) {
    progress_.items_done += items;
    emit(false);
}

void Context::set_current(const Path& path) { progress_.current = path; }

std::chrono::milliseconds Context::active_elapsed() const {
    const auto d = Clock::now() - start_ - excluded_;
    return std::max(std::chrono::milliseconds{0},
                    std::chrono::duration_cast<std::chrono::milliseconds>(d));
}

void Context::update_rate() {
    const auto now = active_elapsed();
    if (samples_.empty() || now - samples_.back().first >= kSampleEvery) {
        samples_.emplace_back(now, progress_.bytes_done);
    }
    while (samples_.size() > 2 && now - samples_.front().first > kRateWindow) {
        samples_.pop_front();
    }
    progress_.bytes_per_second = 0.0;
    progress_.eta.reset();
    if (now < kRateWarmup || samples_.size() < 2) {
        return;
    }
    const auto& first = samples_.front();
    const auto span = now - first.first;
    if (span < std::chrono::milliseconds{1000} || progress_.bytes_done < first.second) {
        return;
    }
    const double seconds = static_cast<double>(span.count()) / 1000.0;
    progress_.bytes_per_second = static_cast<double>(progress_.bytes_done - first.second) / seconds;
    if (progress_.totals_final && progress_.bytes_per_second > 0.0 &&
        progress_.bytes_total >= progress_.bytes_done) {
        const auto left = static_cast<double>(progress_.bytes_total - progress_.bytes_done);
        progress_.eta = std::chrono::seconds{
            static_cast<std::int64_t>(std::ceil(left / progress_.bytes_per_second))};
    }
}

void Context::emit(bool force) {
    if (!sink_) {
        return;
    }
    const auto now = Clock::now();
    if (!force && emitted_once_ && now - last_emit_ < options_.progress_interval) {
        return;
    }
    update_rate();
    progress_.elapsed = active_elapsed();
    last_emit_ = now;
    emitted_once_ = true;
    sink_(progress_);
}

bool Context::inject(const Path& path) const {
    return options_.fault_injector && !options_.fault_injector(path);
}

ConflictDecision Context::ask(Conflict conflict, Shape shape) {
    const auto idx = static_cast<std::size_t>(shape);
    if (const auto& remembered = remembered_.at(idx)) {
        const ConflictAction a = *remembered;
        const bool allowed = (a != ConflictAction::Replace || conflict.replace_allowed) &&
                             (a != ConflictAction::Merge || conflict.merge_allowed);
        if (allowed) {
            return ConflictDecision{a, true, {}};
        }
    }
    conflict.id = ConflictId{next_conflict_++};
    conflict.op = progress_.id;
    conflict.kind = request_.kind;
    conflict.remaining_items = remaining_items();
    ConflictDecision decision{ConflictAction::Skip, false, {}};
    if (resolve_) {
        const OpPhase before = progress_.phase;
        progress_.phase = OpPhase::Waiting;
        emit(true);
        const auto t0 = Clock::now();
        decision = resolve_(conflict);
        excluded_ += Clock::now() - t0;
        samples_.clear();
        progress_.phase = before;
        emit(true);
    }
    if (decision.apply_to_all && decision.action != ConflictAction::Cancel) {
        remembered_.at(idx) = decision.action;
    }
    return decision;
}

void Context::add_steps(std::vector<UndoStep>&& steps) {
    for (UndoStep& s : steps) {
        steps_.push_back(std::move(s));
    }
    steps.clear();
}

void Context::add_touched(const Path& folder) { push_unique(result_.touched_folders, folder); }

void Context::finish_item(ItemOutcome outcome) {
    switch (outcome.status) {
    case ItemStatus::Done: ++result_.done; break;
    case ItemStatus::Skipped: ++result_.skipped; break;
    case ItemStatus::Failed:
        ++result_.failed;
        ++progress_.failed_so_far;
        break;
    case ItemStatus::Changed: ++result_.changed; break;
    case ItemStatus::Cancelled:
    case ItemStatus::Conflict: break;
    }
    ++progress_.top_level_done;
    if (hooks_ != nullptr && hooks_->item_finished) {
        hooks_->item_finished(outcome);
    }
    result_.items.push_back(std::move(outcome));
    emit(false);
}

OpResult Context::finish(OpKind record_kind, const Path& record_destination) {
    result_.cancelled = cancelled();
    const bool recordable =
        !request_.options.dry_run && request_.kind != OpKind::DeletePermanently && !steps_.empty();
    if (recordable) {
        UndoRecord rec;
        rec.kind = record_kind;
        rec.destination = record_destination;
        rec.finished_at = std::chrono::system_clock::now();
        for (const UndoStep& s : steps_) {
            if (s.replaced_permanently) {
                rec.lossy = true;
            }
            push_unique(rec.volumes, record_volume(s.after.path.empty() ? s.before : s.after.path,
                                                   options_.protected_paths));
            if (!s.before.empty()) {
                push_unique(rec.volumes, record_volume(s.before, options_.protected_paths));
            }
        }
        rec.steps = std::move(steps_);
        result_.record = std::move(rec);
    }
    progress_.phase = OpPhase::Done;
    progress_.current.clear();
    emit(true);
    return std::move(result_);
}

PhaseScope::PhaseScope(Context& ctx, OpPhase phase) : ctx_(ctx), previous_(ctx.phase()) {
    ctx_.set_phase(phase);
}

PhaseScope::~PhaseScope() { ctx_.set_phase(previous_); }

} // namespace navigator::ops::detail
