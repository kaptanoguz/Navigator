#include <cstdint>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/commands.hpp>

using navigator::Command;
using navigator::CommandId;
using navigator::CommandQueue;

TEST_CASE("new queue is empty", "[queue]") {
    const CommandQueue queue;
    CHECK(queue.empty());
}

TEST_CASE("drain runs commands in FIFO order", "[queue]") {
    CommandQueue queue;
    queue.enqueue(CommandId::ToggleTheme, 1);
    queue.enqueue(CommandId::Quit, 2);
    queue.enqueue(Command{CommandId::ToggleFpsOverlay, 3});
    queue.enqueue(CommandId::ToggleTheme, 4);
    REQUIRE(queue.size() == 4);

    std::vector<Command> seen;
    queue.drain([&](const Command& c) { seen.push_back(c); });

    REQUIRE(seen.size() == 4);
    CHECK(seen[0].id == CommandId::ToggleTheme);
    CHECK(seen[1].id == CommandId::Quit);
    CHECK(seen[2].id == CommandId::ToggleFpsOverlay);
    CHECK(seen[3].id == CommandId::ToggleTheme);
    for (std::size_t i = 0; i < seen.size(); ++i) {
        CHECK(seen[i].arg == static_cast<std::uint64_t>(i + 1));
    }
}

TEST_CASE("arg defaults to zero", "[queue]") {
    CommandQueue queue;
    queue.enqueue(CommandId::Quit);
    const std::vector<Command> taken = queue.take();
    REQUIRE(taken.size() == 1);
    CHECK(taken[0].arg == 0);
}

TEST_CASE("drain clears the queue", "[queue]") {
    CommandQueue queue;
    queue.enqueue(CommandId::Quit);
    queue.enqueue(CommandId::ToggleTheme);
    int calls = 0;
    queue.drain([&](const Command&) { ++calls; });
    CHECK(calls == 2);
    CHECK(queue.empty());

    calls = 0;
    queue.drain([&](const Command&) { ++calls; });
    CHECK(calls == 0);
}

TEST_CASE("commands enqueued during drain run on the next drain", "[queue]") {
    CommandQueue queue;
    queue.enqueue(CommandId::ToggleTheme);

    std::vector<CommandId> first;
    queue.drain([&](const Command& c) {
        first.push_back(c.id);
        queue.enqueue(CommandId::Quit, 7);
        queue.enqueue(CommandId::ToggleFpsOverlay);
    });
    REQUIRE(first.size() == 1);
    CHECK(first[0] == CommandId::ToggleTheme);
    REQUIRE(queue.size() == 2);

    std::vector<Command> second;
    queue.drain([&](const Command& c) { second.push_back(c); });
    REQUIRE(second.size() == 2);
    CHECK(second[0].id == CommandId::Quit);
    CHECK(second[0].arg == 7);
    CHECK(second[1].id == CommandId::ToggleFpsOverlay);
    CHECK(queue.empty());
}

TEST_CASE("take moves everything out in FIFO order and leaves the queue empty", "[queue]") {
    CommandQueue queue;
    queue.enqueue(CommandId::Quit, 1);
    queue.enqueue(CommandId::ToggleTheme, 2);

    const std::vector<Command> taken = queue.take();
    CHECK(queue.empty());
    REQUIRE(taken.size() == 2);
    CHECK(taken[0].id == CommandId::Quit);
    CHECK(taken[1].id == CommandId::ToggleTheme);

    CHECK(queue.take().empty());

    // Queue is reusable after take().
    queue.enqueue(CommandId::ToggleFpsOverlay);
    CHECK(queue.size() == 1);
}

TEST_CASE("enqueuing None is allowed", "[queue]") {
    CommandQueue queue;
    queue.enqueue(CommandId::None);
    CHECK(queue.size() == 1);
    int calls = 0;
    queue.drain([&](const Command& c) {
        CHECK(c.id == CommandId::None);
        ++calls;
    });
    CHECK(calls == 1);
}
