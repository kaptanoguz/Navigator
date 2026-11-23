// ops::IOperationQueue (navigator/ops.hpp): background workers running run_operation_ex().
//
// One mutex guards everything; public members only take it (never block on I/O). Workers pick
// the oldest queued operation whose path set overlaps no EARLIER unfinished operation
// (submission order, is_same_or_inside either way), so overlapping operations run strictly in
// submission order and the rest run up to max_concurrent at a time. A worker waiting for a
// conflict decision or paused keeps its slot. Events are appended under the lock and drained
// by poll(); Progress events are coalesced while they are the newest event of their operation.
// Workers hold the shared State, so a worker detached by shutdown() (stuck in a blocking call on
// a hung mount) never touches freed memory; on_event is dropped at shutdown.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <stop_token>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <navigator/ops.hpp>

#include "internal.hpp"

namespace navigator::ops {
namespace {

using detail::RunHooks;

struct OpState {
    OpId id;
    OpRequest request;
    EngineOptions engine;
    std::vector<Path> paths;
    std::stop_source stop;
    std::atomic<bool> paused{false};
    bool running = false;
    OpProgress progress;
    std::unique_ptr<Conflict> pending;        // raised, not yet withdrawn
    std::unique_ptr<ConflictDecision> answer; // resolve()'s answer to `pending`
};

struct State {
    mutable std::mutex mu;
    std::condition_variable work_cv;
    std::condition_variable op_cv;
    std::condition_variable exit_cv;
    std::deque<std::shared_ptr<OpState>> ops; // unfinished, submission order
    std::deque<OpEvent> events;
    std::uint64_t next_op = 1;
    std::uint64_t next_conflict = 1;
    std::vector<ConflictId> conflict_order;
    EngineOptions base;
    std::function<void()> on_event;
    bool shutting_down = false;
    std::size_t workers = 0;
    std::size_t exited = 0;
    std::vector<bool> worker_exited;
};

[[nodiscard]] bool overlaps(const std::vector<Path>& a, const std::vector<Path>& b) {
    for (const Path& x : a) {
        for (const Path& y : b) {
            if (is_same_or_inside(x, y) || is_same_or_inside(y, x)) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] std::size_t top_level_count(const OpRequest& r) {
    switch (r.kind) {
    case OpKind::Restore: return r.trashed.size();
    case OpKind::DeletePermanently: return r.sources.size() + r.trashed.size();
    case OpKind::MakeDirectory: return 1;
    case OpKind::Undo:
    case OpKind::Redo: return r.record ? r.record->steps.size() : 0;
    default: return r.sources.size();
    }
}

// Caller holds the lock. Appends `ev`, coalescing a Progress event with the operation's newest
// event when that one is a Progress event too.
void push_event(State& s, OpEvent ev) {
    if (ev.type == OpEvent::Type::Progress) {
        for (OpEvent& last : std::views::reverse(s.events)) {
            if (last.op == ev.op) {
                if (last.type == OpEvent::Type::Progress) {
                    last.progress = std::move(ev.progress);
                    return;
                }
                break;
            }
        }
    }
    s.events.push_back(std::move(ev));
}

void notify(const std::function<void()>& on_event) {
    if (on_event) {
        on_event();
    }
}

// Caller holds the lock: the first runnable operation, or nullptr.
std::shared_ptr<OpState> pick(State& s) {
    for (std::size_t i = 0; i < s.ops.size(); ++i) {
        const auto& op = s.ops[i];
        if (op->running) {
            continue;
        }
        if (op->stop.stop_requested()) {
            return op; // cancelled while queued: finish it at once
        }
        bool blocked = false;
        for (std::size_t j = 0; j < i && !blocked; ++j) {
            blocked = overlaps(s.ops[j]->paths, op->paths);
        }
        if (!blocked) {
            return op;
        }
    }
    return nullptr;
}

void run_one(const std::shared_ptr<State>& sp, const std::shared_ptr<OpState>& op,
             std::unique_lock<std::mutex>& lock) {
    State& s = *sp;
    op->running = true;
    std::shared_ptr<const OpResult> result;
    if (op->stop.stop_requested()) {
        result = std::make_shared<const OpResult>(
            detail::failed_result(op->request, Error{ErrorCode::Cancelled, "cancelled"}, true));
    } else {
        op->progress.phase = OpPhase::Scanning;
        OpEvent started;
        started.type = OpEvent::Type::Started;
        started.op = op->id;
        started.progress = op->progress;
        push_event(s, std::move(started));
        auto on_event = s.on_event;
        lock.unlock();
        notify(on_event);

        RunHooks hooks;
        hooks.op = op->id;
        hooks.pause_requested = [op] { return op->paused.load(std::memory_order_relaxed); };
        hooks.wait_while_paused = [sp, op] {
            std::unique_lock l(sp->mu);
            op->progress.phase = OpPhase::Paused;
            sp->op_cv.wait(l, [&] { return !op->paused.load() || op->stop.stop_requested(); });
        };
        hooks.item_finished = [sp, op](const ItemOutcome& item) {
            std::function<void()> cb;
            {
                const std::lock_guard l(sp->mu);
                OpEvent ev;
                ev.type = OpEvent::Type::ItemFinished;
                ev.op = op->id;
                ev.item = item;
                push_event(*sp, std::move(ev));
                cb = sp->on_event;
            }
            notify(cb);
        };
        const ProgressSink sink = [sp, op](const OpProgress& p) {
            std::function<void()> cb;
            {
                const std::lock_guard l(sp->mu);
                op->progress = p;
                OpEvent ev;
                ev.type = OpEvent::Type::Progress;
                ev.op = op->id;
                ev.progress = p;
                push_event(*sp, std::move(ev));
                cb = sp->on_event;
            }
            notify(cb);
        };
        const ConflictResolver resolver = [sp, op](const Conflict& c) {
            std::unique_lock l(sp->mu);
            Conflict raised = c;
            raised.id = ConflictId{sp->next_conflict++};
            raised.op = op->id;
            op->pending = std::make_unique<Conflict>(raised);
            op->answer.reset();
            sp->conflict_order.push_back(raised.id);
            OpEvent ev;
            ev.type = OpEvent::Type::ConflictRaised;
            ev.op = op->id;
            ev.conflict = raised;
            push_event(*sp, std::move(ev));
            auto cb = sp->on_event;
            l.unlock();
            notify(cb);
            l.lock();
            sp->op_cv.wait(l, [&] { return op->answer != nullptr || op->stop.stop_requested(); });
            std::erase(sp->conflict_order, raised.id);
            op->pending.reset();
            ConflictDecision d{ConflictAction::Cancel, false, {}};
            if (op->answer && !op->stop.stop_requested()) {
                d = *op->answer;
            }
            op->answer.reset();
            return d;
        };

        Result<OpResult> r = [&]() -> Result<OpResult> {
            try {
                return detail::run_operation_ex(op->request, op->engine, resolver, sink,
                                                op->stop.get_token(), &hooks);
            } catch (const std::bad_alloc&) {
                return fail(ErrorCode::OutOfMemory, "out of memory");
            } catch (...) {
                return fail(ErrorCode::Unknown, "unexpected exception");
            }
        }();
        OpResult res = r ? std::move(*r)
                         : detail::failed_result(op->request, r.error(),
                                                 r.error().code == ErrorCode::Cancelled);
        res.id = op->id;
        res.kind = op->request.kind;
        res.origin = op->request.origin;
        result = std::make_shared<const OpResult>(std::move(res));
        lock.lock();
    }
    OpEvent fin;
    fin.type = OpEvent::Type::Finished;
    fin.op = op->id;
    fin.result = std::move(result);
    push_event(s, std::move(fin));
    std::erase(s.ops, op);
    auto cb = s.on_event;
    lock.unlock();
    notify(cb);
    s.work_cv.notify_all();
    lock.lock();
}

void worker_main(const std::shared_ptr<State>& sp, std::size_t index) {
    State& s = *sp;
    std::unique_lock lock(s.mu);
    for (;;) {
        std::shared_ptr<OpState> op;
        s.work_cv.wait(lock, [&] {
            op = pick(s);
            if (op) {
                return true;
            }
            if (!s.shutting_down) {
                return false;
            }
            // Shutting down: leave once nothing queued is left for this worker.
            return std::none_of(s.ops.begin(), s.ops.end(),
                                [](const auto& o) { return !o->running; });
        });
        if (!op) {
            break;
        }
        run_one(sp, op, lock);
    }
    ++s.exited;
    s.worker_exited[index] = true;
    s.exit_cv.notify_all();
}

class OperationQueue final : public IOperationQueue {
public:
    explicit OperationQueue(std::shared_ptr<State> state) : s_(std::move(state)) {}
    ~OperationQueue() override { shutdown(); }

    OperationQueue(const OperationQueue&) = delete;
    OperationQueue& operator=(const OperationQueue&) = delete;
    OperationQueue(OperationQueue&&) = delete;
    OperationQueue& operator=(OperationQueue&&) = delete;

    void start(std::size_t n) {
        s_->workers = n;
        s_->worker_exited.assign(n, false);
        for (std::size_t i = 0; i < n; ++i) {
            threads_.emplace_back([sp = s_, i] { worker_main(sp, i); });
        }
    }

    Result<OpId> submit(OpRequest request) override {
        try {
            const std::lock_guard lock(s_->mu);
            if (s_->shutting_down) {
                return fail(ErrorCode::Cancelled, "the queue is shut down");
            }
            if (auto v = detail::validate_request(request, s_->base.protected_paths,
                                                  s_->base.trash.get());
                !v) {
                return Unexpected<Error>(v.error());
            }
            auto op = std::make_shared<OpState>();
            op->id = OpId{s_->next_op++};
            op->paths = detail::request_paths(request);
            op->engine = s_->base;
            op->progress.id = op->id;
            op->progress.kind = request.kind;
            op->progress.origin = request.origin;
            op->progress.phase = OpPhase::Queued;
            op->progress.destination = request.destination;
            op->progress.top_level_total = top_level_count(request);
            op->request = std::move(request);
            const OpId id = op->id;
            s_->ops.push_back(std::move(op));
            s_->work_cv.notify_all();
            return id;
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "out of memory");
        }
    }

    Result<void> resolve(ConflictId conflict, ConflictDecision decision) override {
        const std::lock_guard lock(s_->mu);
        for (const auto& op : s_->ops) {
            if (!op->pending || !(op->pending->id == conflict) || op->answer) {
                continue;
            }
            const Conflict& c = *op->pending;
            if ((decision.action == ConflictAction::Replace && !c.replace_allowed) ||
                (decision.action == ConflictAction::Merge && !c.merge_allowed) ||
                (decision.action == ConflictAction::KeepBoth && !decision.new_name.empty() &&
                 validate_new_name(decision.new_name) != NameError::None)) {
                return fail(ErrorCode::InvalidArgument, "decision not allowed for this conflict");
            }
            op->answer = std::make_unique<ConflictDecision>(std::move(decision));
            s_->op_cv.notify_all();
            return ok();
        }
        return fail(ErrorCode::NotFound, "no such pending conflict");
    }

    void cancel(OpId op) noexcept override {
        const std::lock_guard lock(s_->mu);
        for (const auto& o : s_->ops) {
            if (o->id == op) {
                o->stop.request_stop();
            }
        }
        s_->op_cv.notify_all();
        s_->work_cv.notify_all();
    }

    void cancel_all() noexcept override {
        const std::lock_guard lock(s_->mu);
        for (const auto& o : s_->ops) {
            o->stop.request_stop();
        }
        s_->op_cv.notify_all();
        s_->work_cv.notify_all();
    }

    void pause(OpId op) noexcept override {
        const std::lock_guard lock(s_->mu);
        for (const auto& o : s_->ops) {
            if (o->id == op && o->running) {
                o->paused.store(true);
            }
        }
    }

    void resume(OpId op) noexcept override {
        const std::lock_guard lock(s_->mu);
        for (const auto& o : s_->ops) {
            if (o->id == op) {
                o->paused.store(false);
            }
        }
        s_->op_cv.notify_all();
    }

    std::size_t poll(std::vector<OpEvent>& out) override {
        const std::lock_guard lock(s_->mu);
        const std::size_t n = s_->events.size();
        for (OpEvent& e : s_->events) {
            out.push_back(std::move(e));
        }
        s_->events.clear();
        return n;
    }

    std::vector<OpProgress> snapshot() const override {
        const std::lock_guard lock(s_->mu);
        std::vector<OpProgress> out;
        out.reserve(s_->ops.size());
        for (const auto& o : s_->ops) {
            out.push_back(o->progress);
        }
        return out;
    }

    std::vector<Conflict> pending_conflicts() const override {
        const std::lock_guard lock(s_->mu);
        std::vector<Conflict> out;
        for (const ConflictId id : s_->conflict_order) {
            for (const auto& o : s_->ops) {
                if (o->pending && o->pending->id == id && !o->answer) {
                    out.push_back(*o->pending);
                }
            }
        }
        return out;
    }

    void set_protected_paths(ProtectedPaths paths) override {
        const std::lock_guard lock(s_->mu);
        s_->base.protected_paths = std::move(paths);
    }

    void shutdown() noexcept override {
        {
            std::unique_lock lock(s_->mu);
            if (!s_->shutting_down) {
                s_->shutting_down = true;
                for (const auto& o : s_->ops) {
                    o->stop.request_stop();
                }
            }
            s_->op_cv.notify_all();
            s_->work_cv.notify_all();
            // Each worker stops within one chunk; a worker stuck in a blocking call (hung
            // network mount) is detached after 2 s.
            s_->exit_cv.wait_for(lock, std::chrono::seconds(2),
                                 [&] { return s_->exited == s_->workers; });
            s_->on_event = nullptr; // a detached worker must not call into the ui
        }
        for (std::size_t i = 0; i < threads_.size(); ++i) {
            if (!threads_[i].joinable()) {
                continue;
            }
            bool exited = false;
            {
                const std::lock_guard lock(s_->mu);
                exited = s_->worker_exited[i];
            }
            if (exited) {
                threads_[i].join();
            } else {
                threads_[i].detach();
            }
        }
    }

private:
    std::shared_ptr<State> s_;
    std::vector<std::thread> threads_;
};

} // namespace

Result<std::unique_ptr<IOperationQueue>> create_operation_queue(QueueOptions options) {
    if (options.max_concurrent > 8) {
        return fail(ErrorCode::InvalidArgument, "max_concurrent must be 1..8");
    }
    const std::size_t n = options.max_concurrent == 0 ? 2 : options.max_concurrent;
    try {
        auto state = std::make_shared<State>();
        state->base = std::move(options.engine);
        state->on_event = std::move(options.on_event);
        auto q = std::make_unique<OperationQueue>(state);
        try {
            q->start(n);
        } catch (const std::system_error& e) {
            return fail(ErrorCode::PlatformError, std::string("cannot start workers: ") + e.what());
        }
        return std::unique_ptr<IOperationQueue>(std::move(q));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory");
    }
}

} // namespace navigator::ops
