// src/ops/context.hpp: per-run state of the engine: request, cancellation and pause checks (S9),
// progress with a sliding-window rate (OpProgress), the conflict handshake with "apply to all"
// memory, undo steps and the OpResult being built.
//
// Thread-safety: one Context per run, used by the running thread only.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <navigator/ops.hpp>

#include "internal.hpp"

namespace navigator::ops::detail {

// Shape of a conflict for ConflictDecision::apply_to_all.
enum class Shape : std::uint8_t { FileOntoFile = 0, DirOntoDir = 1, Mixed = 2 };

class Context {
public:
    Context(const OpRequest& request, const EngineOptions& options, const ConflictResolver& resolve,
            const ProgressSink& sink, std::stop_token stop, const RunHooks* hooks);

    [[nodiscard]] const OpRequest& request() const noexcept { return request_; }
    [[nodiscard]] const OpOptions& opt() const noexcept { return request_.options; }
    [[nodiscard]] const EngineOptions& engine() const noexcept { return options_; }
    [[nodiscard]] trash::ITrash* trash() const noexcept { return options_.trash.get(); }
    [[nodiscard]] OpKind kind() const noexcept { return request_.kind; }
    [[nodiscard]] bool is_undo_or_redo() const noexcept {
        return request_.kind == OpKind::Undo || request_.kind == OpKind::Redo;
    }
    [[nodiscard]] std::size_t chunk() const noexcept;

    // Cancellation and pause (S9). checkpoint() blocks while paused and returns !cancelled().
    [[nodiscard]] bool cancelled() const noexcept;
    [[nodiscard]] bool checkpoint();
    void cancel() noexcept { internal_cancel_ = true; }

    // Progress.
    void set_phase(OpPhase phase);
    [[nodiscard]] OpPhase phase() const noexcept { return progress_.phase; }
    void add_total(std::uint64_t items, std::uint64_t bytes);
    void set_totals_final();
    void add_bytes(std::uint64_t bytes);
    void add_items(std::uint64_t items = 1);
    void set_current(const Path& path);
    void count_failure() { ++progress_.failed_so_far; }
    void emit(bool force);

    // EngineOptions::fault_injector: true when `path` must fail with IoError ("injected").
    [[nodiscard]] bool inject(const Path& path) const;

    // Raises `conflict` (id / op / kind / remaining filled in here) unless "apply to all" already
    // answers it. Empty resolver: Skip.
    [[nodiscard]] ConflictDecision ask(Conflict conflict, Shape shape);

    void add_step(UndoStep step) { steps_.push_back(std::move(step)); }
    void add_steps(std::vector<UndoStep>&& steps);
    void add_touched(const Path& folder);
    void warn_metadata(std::size_t n = 1) { result_.metadata_warnings += n; }
    void add_untrashable(const Path& path) { result_.untrashable.push_back(path); }
    void finish_item(ItemOutcome outcome);
    void set_top_level_total(std::size_t n) { progress_.top_level_total = n; }
    [[nodiscard]] std::size_t remaining_items() const noexcept {
        return progress_.top_level_total > progress_.top_level_done
                   ? progress_.top_level_total - progress_.top_level_done
                   : 0;
    }

    // Builds the final OpResult (record from the collected steps unless dry_run /
    // DeletePermanently). `record_kind` is the forward kind for Undo / Redo records.
    [[nodiscard]] OpResult finish(OpKind record_kind, const Path& record_destination);

private:
    using Clock = std::chrono::steady_clock;
    [[nodiscard]] std::chrono::milliseconds active_elapsed() const;
    void update_rate();

    const OpRequest& request_;
    const EngineOptions& options_;
    const ConflictResolver& resolve_;
    const ProgressSink& sink_;
    std::stop_token stop_;
    const RunHooks* hooks_;
    bool internal_cancel_ = false;

    OpProgress progress_;
    Clock::time_point start_;
    Clock::duration excluded_{};
    Clock::time_point last_emit_{};
    bool emitted_once_ = false;
    std::deque<std::pair<std::chrono::milliseconds, std::uint64_t>> samples_;

    std::array<std::optional<ConflictAction>, 3> remembered_{};
    std::uint64_t next_conflict_ = 1;

    std::vector<UndoStep> steps_;
    OpResult result_;
};

// RAII: sets a phase and restores the previous one.
class PhaseScope {
public:
    PhaseScope(Context& ctx, OpPhase phase);
    PhaseScope(const PhaseScope&) = delete;
    PhaseScope& operator=(const PhaseScope&) = delete;
    PhaseScope(PhaseScope&&) = delete;
    PhaseScope& operator=(PhaseScope&&) = delete;
    ~PhaseScope();

private:
    Context& ctx_;
    OpPhase previous_;
};

} // namespace navigator::ops::detail
