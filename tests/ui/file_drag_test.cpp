// File drag and drop as pure logic (Phase 7): drop slot priority, the drop action decision with
// GNOME Files modifiers, the internal drag state machine (hover, spring-loading, drop, cancel)
// and the incoming drop session (SDL drop events). Filesystem facts come from a fake probe, plus
// one test on a fixture directory for the default probe.
#include <map>

#include <catch2/catch_test_macros.hpp>

#include <navigator/transfer.hpp>

#include "dnd/file_drag.hpp"
#include "ui_fixture.hpp"

using namespace navigator;
using namespace navigator::ui;
using transfer::DropAction;

namespace {

class FakeProbe final : public DropProbe {
public:
    std::map<std::string, std::uint64_t> devices;
    std::map<std::string, bool> write;
    [[nodiscard]] std::optional<std::uint64_t> device(const Path& p) override {
        const auto it = devices.find(p.string());
        if (it == devices.end()) {
            return std::nullopt;
        }
        return it->second;
    }
    [[nodiscard]] bool writable(const Path& f) override {
        const auto it = write.find(f.string());
        return it == write.end() || it->second;
    }
};

DropSlot slot(SlotKind kind, RectF r, const char* folder, std::uint32_t tab = 1) {
    DropSlot s;
    s.kind = kind;
    s.rect = r;
    s.folder = folder;
    s.tab = tab;
    s.label = Path(folder).filename().string();
    return s;
}

} // namespace

TEST_CASE("The most specific drop slot wins", "[dnd]") {
    std::vector<DropSlot> slots{
        slot(SlotKind::Pane, {0, 0, 400, 400}, "/pane"),
        slot(SlotKind::Row, {10, 100, 380, 24}, "/pane/folder"),
        slot(SlotKind::Crumb, {10, 10, 60, 24}, "/pane/up"),
        slot(SlotKind::Tab, {10, 10, 60, 24}, "/other"), // registered later, same priority tier
    };
    CHECK(pick_slot(slots, 50, 110)->kind == SlotKind::Row);
    CHECK(pick_slot(slots, 300, 300)->kind == SlotKind::Pane);
    CHECK(pick_slot(slots, 30, 20)->folder == "/other"); // later registration wins a tie
    CHECK(pick_slot(slots, 900, 900) == nullptr);
    // The Trash row outranks the sidebar.
    slots.push_back(slot(SlotKind::Sidebar, {500, 0, 100, 30}, "/home"));
    DropSlot trash = slot(SlotKind::Trash, {500, 0, 100, 30}, "");
    slots.push_back(trash);
    slots.push_back(slot(SlotKind::Sidebar, {500, 0, 100, 30}, "/later"));
    CHECK(pick_slot(slots, 510, 10)->kind == SlotKind::Trash);
}

TEST_CASE("Drop action follows GNOME Files conventions", "[dnd]") {
    FakeProbe probe;
    probe.devices = {{"/a/file", 1}, {"/a", 1}, {"/b", 1}, {"/usb", 2}, {"/a/dir", 1}};
    const std::vector<Path> one{"/a/file"};
    const DropSlot b = slot(SlotKind::Row, {0, 0, 10, 10}, "/b");
    const DropSlot usb = slot(SlotKind::Row, {0, 0, 10, 10}, "/usb");
    const DropSlot same = slot(SlotKind::Pane, {0, 0, 10, 10}, "/a");
    // Same filesystem: move; other filesystem: copy.
    CHECK(evaluate_drop(one, &b, true, Modifiers::None, probe).action == DropAction::Move);
    CHECK(evaluate_drop(one, &usb, true, Modifiers::None, probe).action == DropAction::Copy);
    // Modifiers.
    CHECK(evaluate_drop(one, &b, true, Modifiers::Ctrl, probe).action == DropAction::Copy);
    CHECK(evaluate_drop(one, &usb, true, Modifiers::Shift, probe).action == DropAction::Move);
    CHECK(evaluate_drop(one, &b, true, Modifiers::Ctrl | Modifiers::Shift, probe).action ==
          DropAction::Link);
    CHECK(evaluate_drop(one, &b, true, Modifiers::Alt, probe).action == DropAction::Ask);
    // Back onto the source folder without a modifier: nothing.
    CHECK(evaluate_drop(one, &same, true, Modifiers::None, probe).action == DropAction::None);
    CHECK(evaluate_drop(one, &same, true, Modifiers::Ctrl, probe).action == DropAction::Copy);
    // Into itself: never.
    const std::vector<Path> dir{"/a/dir"};
    const DropSlot inside = slot(SlotKind::Row, {0, 0, 10, 10}, "/a/dir/deeper");
    CHECK(evaluate_drop(dir, &inside, true, Modifiers::Ctrl, probe).action == DropAction::None);
    const DropSlot itself = slot(SlotKind::Row, {0, 0, 10, 10}, "/a/dir");
    CHECK(evaluate_drop(dir, &itself, true, Modifiers::None, probe).action == DropAction::None);
    // A read-only target.
    probe.write["/b"] = false;
    CHECK(evaluate_drop(one, &b, true, Modifiers::None, probe).action == DropAction::None);
    probe.write["/b"] = true;
    // External drops copy unless Shift is held.
    CHECK(evaluate_drop(one, &b, false, Modifiers::None, probe).action == DropAction::Copy);
    CHECK(evaluate_drop(one, &b, false, Modifiers::Shift, probe).action == DropAction::Move);
    // The Trash row: only our own items; the folder stays empty.
    const DropSlot t = slot(SlotKind::Trash, {0, 0, 10, 10}, "");
    const DropEval te = evaluate_drop(one, &t, true, Modifiers::None, probe);
    CHECK(te.trash);
    CHECK(te.allowed());
    CHECK_FALSE(evaluate_drop(one, &t, false, Modifiers::None, probe).allowed());
    // No slot, no items.
    CHECK_FALSE(evaluate_drop(one, nullptr, true, Modifiers::None, probe).allowed());
    CHECK_FALSE(evaluate_drop({}, &b, true, Modifiers::None, probe).allowed());
}

TEST_CASE("The default probe reads the filesystem", "[dnd]") {
    test::TempDir t;
    t.dir("A");
    t.dir("B");
    t.file("A/f.txt");
    const DropSlot b = slot(SlotKind::Row, {0, 0, 10, 10}, "");
    DropSlot bs = b;
    bs.folder = t.path() / "B";
    const DropEval e =
        evaluate_drop({t.path() / "A/f.txt"}, &bs, true, Modifiers::None, default_drop_probe());
    CHECK(e.action == DropAction::Move); // one temp directory: one filesystem
    DropSlot missing = b;
    missing.folder = t.path() / "nope";
    CHECK(
        evaluate_drop({t.path() / "A/f.txt"}, &missing, true, Modifiers::None, default_drop_probe())
            .action == DropAction::None); // not writable (does not exist)
}

TEST_CASE("An internal drag hovers, evaluates every frame and drops", "[dnd]") {
    FakeProbe probe;
    probe.devices = {{"/a/x", 1}, {"/b", 1}};
    const std::vector<DropSlot> slots{slot(SlotKind::Pane, {0, 0, 200, 200}, "/a"),
                                      slot(SlotKind::Row, {0, 50, 200, 24}, "/b", 2)};
    FileDrag d;
    CHECK_FALSE(d.dragging());
    d.begin({"/a/x"}, 1, 0.0);
    REQUIRE(d.dragging());
    d.update(100, 60, Modifiers::None, slots, 10.0, probe);
    CHECK(d.hover().value_or(DropSlot{}).folder == "/b");
    CHECK(d.eval().action == DropAction::Move);
    // The modifier changes the action while hovering.
    d.update(100, 60, Modifiers::Ctrl, slots, 20.0, probe);
    CHECK(d.eval().action == DropAction::Copy);
    // Moving off every slot: nothing to drop.
    d.update(900, 900, Modifiers::None, slots, 30.0, probe);
    CHECK_FALSE(d.eval().allowed());
    d.update(100, 60, Modifiers::None, slots, 40.0, probe);
    const FileDrag::Outcome o = d.finish();
    CHECK(o.kind == FileDrag::Outcome::Kind::Drop);
    CHECK(o.items == std::vector<Path>{"/a/x"});
    CHECK(o.eval.folder == "/b");
    CHECK_FALSE(d.dragging());
}

TEST_CASE("Releasing outside a target or on the source folder cancels; Escape cancels", "[dnd]") {
    FakeProbe probe;
    probe.devices = {{"/a/x", 1}, {"/a", 1}};
    const std::vector<DropSlot> slots{slot(SlotKind::Pane, {0, 0, 200, 200}, "/a")};
    FileDrag d;
    d.begin({"/a/x"}, 1, 0.0);
    d.update(50, 50, Modifiers::None, slots, 1.0, probe); // the source folder, no modifier
    CHECK(d.finish().kind == FileDrag::Outcome::Kind::Cancel);
    d.begin({"/a/x"}, 1, 0.0);
    d.update(500, 500, Modifiers::None, slots, 1.0, probe);
    CHECK(d.finish().kind == FileDrag::Outcome::Kind::Cancel);
    d.begin({"/a/x"}, 1, 0.0);
    d.cancel();
    CHECK_FALSE(d.dragging());
    CHECK(d.finish().kind == FileDrag::Outcome::Kind::Cancel);
    d.begin({}, 1, 0.0); // nothing to drag
    CHECK_FALSE(d.dragging());
}

TEST_CASE("A folder row hovered for 700 ms springs open once", "[dnd]") {
    FakeProbe probe;
    probe.devices = {{"/a/x", 1}, {"/b", 1}, {"/c", 1}};
    const std::vector<DropSlot> slots{slot(SlotKind::Row, {0, 0, 100, 24}, "/b", 3),
                                      slot(SlotKind::Row, {0, 30, 100, 24}, "/c", 3),
                                      slot(SlotKind::Pane, {0, 100, 100, 100}, "/a", 3)};
    FileDrag d;
    d.begin({"/a/x"}, 1, 0.0);
    d.update(10, 10, Modifiers::None, slots, 100.0, probe);
    CHECK_FALSE(d.take_spring(500.0).has_value());
    d.update(10, 10, Modifiers::None, slots, 810.0, probe);
    const std::optional<DropSlot> s = d.take_spring(810.0);
    REQUIRE(s.has_value());
    CHECK(s.value_or(DropSlot{}).folder == "/b");
    CHECK(s.value_or(DropSlot{}).tab == 3);
    CHECK_FALSE(d.take_spring(900.0).has_value()); // once per hover
    // A different row restarts the timer.
    d.update(10, 40, Modifiers::None, slots, 1000.0, probe);
    CHECK_FALSE(d.take_spring(1500.0).has_value());
    CHECK(d.take_spring(1710.0).has_value());
    // A pane never springs.
    d.update(10, 150, Modifiers::None, slots, 2000.0, probe);
    CHECK_FALSE(d.take_spring(9000.0).has_value());
}

TEST_CASE("Incoming drops collect files and uri lists from SDL events", "[dnd]") {
    IncomingDrop in;
    CHECK_FALSE(in.active());
    in.begin();
    in.position(10, 20);
    CHECK(in.active());
    CHECK(in.has_position());
    in.add_path("/home/u/a b.txt");
    in.add_path("/home/u/a b.txt"); // duplicates once
    in.add_path("relative.txt");    // refused: not absolute
    CHECK(in.add_text("file:///home/u/%C5%9Fark%C4%B1.txt\r\nhttp://example.com/x\r\n# c\r\n") ==
          1);
    CHECK(in.paths().size() == 2);
    CHECK(in.paths()[1] == Path("/home/u/şarkı.txt"));
    const std::vector<Path> got = in.complete();
    CHECK(got.size() == 2);
    CHECK_FALSE(in.active());
    CHECK(in.paths().empty());
    // Nothing usable arrived (XdndDirectSave and friends).
    in.begin();
    CHECK(in.complete().empty());
}
