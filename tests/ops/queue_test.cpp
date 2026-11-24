// IOperationQueue: validation, events, conflict handshake, serialization of overlapping
// operations, concurrency, pause / resume, cancel, protected paths, shutdown. Fixtures only.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "support.hpp"

using namespace navigator;
using namespace navigator::ops;
using namespace navigator::ops::test;
using namespace std::chrono_literals;

namespace {

struct QueueEnv {
    Fixture fx;
    Env env{fx};
    std::atomic<int> wakeups{0};
    std::unique_ptr<IOperationQueue> q;
    std::vector<OpEvent> events;

    explicit QueueEnv(std::size_t max_concurrent = 2,
                      std::function<bool(const Path&)> inject = {}) {
        QueueOptions o;
        o.engine = env.engine;
        o.engine.fault_injector = [inject = std::move(inject)](const Path& p) {
            require_inside_fixture(p);
            return inject ? inject(p) : true;
        };
        o.engine.progress_interval = 0ms;
        o.max_concurrent = max_concurrent;
        o.on_event = [this] { ++wakeups; };
        auto made = create_operation_queue(std::move(o));
        REQUIRE(made);
        q = std::move(*made);
    }
    [[nodiscard]] Result<OpId> submit(const OpRequest& r) const {
        guard_request(r);
        return q->submit(r);
    }
    // Polls until `op` finished (or the deadline); returns its result.
    std::shared_ptr<const OpResult> wait(OpId op, std::chrono::milliseconds limit = 10000ms) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            const std::size_t from = events.size();
            q->poll(events);
            for (std::size_t i = from; i < events.size(); ++i) {
                if (events[i].type == OpEvent::Type::Finished && events[i].op == op) {
                    return events[i].result;
                }
            }
            for (std::size_t i = 0; i < from; ++i) {
                if (events[i].type == OpEvent::Type::Finished && events[i].op == op) {
                    return events[i].result;
                }
            }
            std::this_thread::sleep_for(1ms);
        }
        return nullptr;
    }
    std::vector<OpEvent> of(OpId op) const {
        std::vector<OpEvent> out;
        for (const OpEvent& e : events) {
            if (e.op == op) {
                out.push_back(e);
            }
        }
        return out;
    }
    // The first pending conflict; throws (failing the test) when none appears in time.
    [[nodiscard]] Conflict wait_conflict(std::chrono::milliseconds limit = 10000ms) const {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            auto c = q->pending_conflicts();
            if (!c.empty()) {
                return c.front();
            }
            std::this_thread::sleep_for(1ms);
        }
        throw std::runtime_error("no conflict was raised");
    }
};

} // namespace

TEST_CASE("queue: submit validation and event order", "[queue]") {
    QueueEnv e;
    CHECK(e.q->submit(copy_req({}, e.fx.root())).error().code == ErrorCode::InvalidArgument);
    OpRequest del = delete_req({e.fx / "x"});
    del.confirmed = false;
    CHECK(e.q->submit(del).error().code == ErrorCode::InvalidArgument);
    e.fx.file("src/a", "a");
    e.fx.dir("dst");
    auto id = e.submit(copy_req({e.fx / "src/a"}, e.fx / "dst"));
    REQUIRE(id);
    CHECK(id->valid());
    auto res = e.wait(*id);
    REQUIRE(res);
    CHECK(res->id == *id);
    CHECK(res->done == 1);
    const auto evs = e.of(*id);
    REQUIRE(evs.size() >= 3);
    CHECK(evs.front().type == OpEvent::Type::Started);
    CHECK(evs.back().type == OpEvent::Type::Finished);
    bool item = false;
    for (const OpEvent& ev : evs) {
        item = item || ev.type == OpEvent::Type::ItemFinished;
        if (ev.type == OpEvent::Type::Progress) {
            REQUIRE(ev.progress);
            CHECK(ev.progress.value_or(OpProgress{}).id == *id);
        }
    }
    CHECK(item);
    CHECK(e.wakeups > 0);
    CHECK(e.q->snapshot().empty());
    CHECK(read_file(e.fx / "dst/a") == "a");
}

TEST_CASE("queue: conflict handshake with resolve()", "[queue][conflict]") {
    QueueEnv e;
    e.fx.file("src/a", "new");
    e.fx.file("dst/a", "old");
    auto id = e.submit(copy_req({e.fx / "src/a"}, e.fx / "dst"));
    REQUIRE(id);
    const Conflict c = e.wait_conflict();
    CHECK(c.op == *id);
    CHECK(c.id.valid());
    auto snap = e.q->snapshot();
    REQUIRE(snap.size() == 1);
    CHECK(snap[0].phase == OpPhase::Waiting);
    CHECK(e.q->resolve(ConflictId{999999}, {}).error().code == ErrorCode::NotFound);
    CHECK(e.q->resolve(c.id, ConflictDecision{ConflictAction::Merge, false, {}}).error().code ==
          ErrorCode::InvalidArgument);
    CHECK(
        e.q->resolve(c.id, ConflictDecision{ConflictAction::KeepBoth, false, "a/b"}).error().code ==
        ErrorCode::InvalidArgument);
    REQUIRE(e.q->resolve(c.id, ConflictDecision{ConflictAction::KeepBoth, false, "kept"}));
    CHECK(e.q->resolve(c.id, {}).error().code == ErrorCode::NotFound); // already answered
    auto res = e.wait(*id);
    REQUIRE(res);
    CHECK(read_file(e.fx / "dst/kept") == "new");
    bool raised = false;
    for (const OpEvent& ev : e.of(*id)) {
        raised = raised || (ev.type == OpEvent::Type::ConflictRaised &&
                            ev.conflict.value_or(Conflict{}).id == c.id);
    }
    CHECK(raised);
}

TEST_CASE("queue: cancel withdraws a pending conflict", "[queue][cancel]") {
    QueueEnv e;
    e.fx.file("src/a", "new");
    e.fx.file("dst/a", "old");
    auto id = e.submit(copy_req({e.fx / "src/a"}, e.fx / "dst"));
    REQUIRE(id);
    const Conflict c = e.wait_conflict();
    e.q->cancel(*id);
    auto res = e.wait(*id);
    REQUIRE(res);
    CHECK(res->cancelled);
    CHECK(e.q->pending_conflicts().empty());
    CHECK(e.q->resolve(c.id, {}).error().code == ErrorCode::NotFound);
    CHECK(read_file(e.fx / "dst/a") == "old");
    e.q->cancel(OpId{424242}); // unknown: no-op
}

TEST_CASE("queue: overlapping operations run in submission order; others concurrently",
          "[queue][order]") {
    // A gate inside the fault injector holds the first operation until released.
    std::mutex mu;
    std::condition_variable cv;
    bool release = false;
    std::atomic<int> inside_gate{0};
    std::vector<std::string> order;
    QueueEnv e(2, [&](const Path& p) {
        const std::string name = p.filename().native();
        {
            const std::lock_guard l(mu);
            order.push_back(name);
        }
        if (name == "gate") {
            ++inside_gate;
            std::unique_lock l(mu);
            cv.wait(l, [&] { return release; });
        }
        return true;
    });
    e.fx.file("shared/gate", "g");
    e.fx.file("shared/second", "s");
    e.fx.file("other/free", "f");
    e.fx.dir("d1");
    e.fx.dir("d2");
    auto first = e.submit(copy_req({e.fx / "shared/gate"}, e.fx / "d1"));
    auto second = e.submit(move_req({e.fx / "shared/second"}, e.fx / "d1")); // overlaps d1
    auto third = e.submit(copy_req({e.fx / "other/free"}, e.fx / "d2"));     // independent
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(third);
    // The independent one finishes while the first is held; the overlapping one waits.
    auto r3 = e.wait(*third);
    REQUIRE(r3);
    CHECK(r3->done == 1);
    CHECK(inside_gate == 1);
    auto snap = e.q->snapshot();
    REQUIRE(snap.size() == 2);
    CHECK(snap[0].id == *first);
    CHECK(snap[1].id == *second);
    CHECK(snap[1].phase == OpPhase::Queued);
    {
        const std::lock_guard l(mu);
        release = true;
    }
    cv.notify_all();
    auto r1 = e.wait(*first);
    auto r2 = e.wait(*second);
    REQUIRE(r1);
    REQUIRE(r2);
    const std::lock_guard l(mu);
    const auto pos = [&](const std::string& n) {
        return std::find(order.begin(), order.end(), n) - order.begin();
    };
    CHECK(pos("gate") < pos("second"));
}

TEST_CASE("queue: max_concurrent bounds running operations", "[queue][order]") {
    std::mutex mu;
    std::condition_variable cv;
    bool release = false;
    std::atomic<int> running{0};
    std::atomic<int> peak{0};
    QueueEnv e(2, [&](const Path& p) {
        if (p.filename().native().starts_with("item")) {
            const int now = ++running;
            int prev = peak.load();
            while (now > prev && !peak.compare_exchange_weak(prev, now)) {
            }
            std::unique_lock l(mu);
            cv.wait_for(l, 200ms, [&] { return release; });
            --running;
        }
        return true;
    });
    std::vector<OpId> ids;
    for (int i = 0; i < 5; ++i) {
        const std::string n = "item" + std::to_string(i);
        e.fx.file("s" + std::to_string(i) + "/" + n, n);
        e.fx.dir("d" + std::to_string(i));
        auto id = e.submit(
            copy_req({e.fx / ("s" + std::to_string(i)) / n}, e.fx / ("d" + std::to_string(i))));
        REQUIRE(id);
        ids.push_back(*id);
    }
    for (const OpId id : ids) {
        REQUIRE(e.wait(id));
    }
    CHECK(peak.load() == 2);
    {
        const std::lock_guard l(mu);
        release = true;
    }
    cv.notify_all();
    CHECK(create_operation_queue(QueueOptions{{}, 9, {}}).error().code ==
          ErrorCode::InvalidArgument);
}

TEST_CASE("queue: pause holds an operation, resume continues it", "[queue][pause]") {
    std::atomic<bool> started{false};
    std::atomic<bool> go_on{false};
    QueueEnv e(2, [&](const Path& p) {
        if (p.filename() == "b") {
            started = true;
            while (!go_on) {
                std::this_thread::sleep_for(1ms);
            }
        }
        return true;
    });
    e.fx.file("src/a", "a");
    e.fx.file("src/b", "b");
    e.fx.file("src/c", "c");
    e.fx.dir("dst");
    auto id = e.submit(copy_req({e.fx / "src/a", e.fx / "src/b", e.fx / "src/c"}, e.fx / "dst"));
    REQUIRE(id);
    while (!started) {
        std::this_thread::sleep_for(1ms);
    }
    e.q->pause(*id);
    go_on = true;
    // It stops at the next check and holds.
    bool paused = false;
    for (int i = 0; i < 2000 && !paused; ++i) {
        auto snap = e.q->snapshot();
        paused = !snap.empty() && snap[0].phase == OpPhase::Paused;
        std::this_thread::sleep_for(1ms);
    }
    CHECK(paused);
    std::this_thread::sleep_for(30ms);
    CHECK_FALSE(present(e.fx / "dst/c"));
    e.q->resume(*id);
    auto res = e.wait(*id);
    REQUIRE(res);
    CHECK(res->done == 3);
    CHECK(read_file(e.fx / "dst/c") == "c");
}

TEST_CASE("queue: set_protected_paths applies to later submissions", "[queue][refuse]") {
    QueueEnv e;
    const Path home = e.fx.dir("home");
    e.fx.dir("dst");
    ProtectedPaths pp;
    pp.home = home;
    e.q->set_protected_paths(pp);
    CHECK(e.q->submit(trash_req({home})).error().code == ErrorCode::PermissionDenied);
    CHECK(e.q->submit(move_req({home}, e.fx / "dst")).error().code == ErrorCode::PermissionDenied);
    auto ok_id = e.submit(copy_req({home}, e.fx / "dst"));
    REQUIRE(ok_id);
    REQUIRE(e.wait(*ok_id));
}

TEST_CASE("queue: shutdown cancels running and queued operations, then refuses", "[queue]") {
    std::atomic<bool> hold{true};
    auto e = std::make_unique<QueueEnv>(1, [&](const Path& p) {
        if (p.filename() == "slow") {
            for (int i = 0; i < 200 && hold; ++i) {
                std::this_thread::sleep_for(5ms);
            }
        }
        return true;
    });
    e->fx.file("s1/slow", std::string(100000, 's'));
    e->fx.file("s2/x", "x");
    e->fx.dir("dst");
    auto a = e->submit(copy_req({e->fx / "s1/slow"}, e->fx / "dst"));
    auto b = e->submit(copy_req({e->fx / "s2/x"}, e->fx / "dst"));
    REQUIRE(a);
    REQUIRE(b);
    std::this_thread::sleep_for(20ms);
    e->q->shutdown();
    hold = false;
    e->q->shutdown(); // idempotent
    std::vector<OpEvent> evs;
    e->q->poll(evs);
    int finished = 0;
    for (const OpEvent& ev : evs) {
        if (ev.type == OpEvent::Type::Finished) {
            ++finished;
            REQUIRE(ev.result);
        }
    }
    CHECK(finished == 2);
    CHECK(e->q->submit(copy_req({e->fx / "s2/x"}, e->fx / "dst")).error().code ==
          ErrorCode::Cancelled);
    CHECK(part_files(e->fx.root()).empty());
}

TEST_CASE("queue: undo through the queue and the UndoStack", "[queue][undo]") {
    QueueEnv e;
    UndoStack stack;
    e.fx.file("src/a", "a");
    e.fx.dir("dst");
    auto id = e.submit(move_req({e.fx / "src/a"}, e.fx / "dst"));
    REQUIRE(id);
    auto res = e.wait(*id);
    REQUIRE(res);
    REQUIRE(res->record);
    stack.record_done(record_of(*res), OpOrigin::User);
    auto rec = stack.take_undo();
    OpRequest u;
    u.kind = OpKind::Undo;
    u.origin = OpOrigin::Undo;
    u.record = rec;
    auto uid = e.submit(u);
    REQUIRE(uid);
    auto ures = e.wait(*uid);
    REQUIRE(ures);
    CHECK(ures->origin == OpOrigin::Undo);
    CHECK(read_file(e.fx / "src/a") == "a");
    REQUIRE(ures->record);
    stack.record_done(record_of(*ures), OpOrigin::Undo);
    CHECK(stack.can_redo());
}
