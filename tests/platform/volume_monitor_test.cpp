// IVolumeMonitor: fake sources without change_fd (re-read on every poll), a fixture-file proc
// source (change_fd present, text diff), and the real /proc.
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/volume.hpp>

#include "fake_mounts.hpp"

namespace {

using navigator::VolumeEvent;
using navigator::VolumeEventKind;
using navigator::platform_test::FakeMountSource;
using navigator::platform_test::FakeSystem;
using navigator::platform_test::write_file;
using namespace std::chrono_literals;

const std::string kBase = "22 1 259:2 / / rw - ext4 /dev/nvme0n1p2 rw\n"
                          "23 22 0:5 / /proc rw - proc proc rw\n";
const std::string kUsb = "29 22 8:17 / /media/u/STICK rw - vfat /dev/sdb1 rw\n";
const std::string kUsbRo = "29 22 8:17 / /media/u/STICK ro - vfat /dev/sdb1 ro\n";
const std::string kUsbRemounted = "31 22 8:17 / /media/u/STICK rw - vfat /dev/sdb1 rw\n";

std::vector<VolumeEvent> poll_ok(navigator::IVolumeMonitor& m) {
    std::vector<VolumeEvent> out;
    auto n = m.poll(out);
    REQUIRE(n.has_value());
    REQUIRE(*n == out.size());
    return out;
}

} // namespace

TEST_CASE("monitor diffs snapshots into Mounted / Unmounted / Changed", "[volume][monitor]") {
    FakeSystem sys;
    auto source = std::make_shared<FakeMountSource>(kBase);
    auto created = navigator::create_volume_monitor(sys.query(source));
    REQUIRE(created.has_value());
    auto& m = **created;
    CHECK(m.native_handle() == -1);
    REQUIRE(m.volumes().size() == 1); // /proc filtered
    CHECK(m.volumes()[0].mount_point == "/");

    CHECK(poll_ok(m).empty()); // nothing changed, no events for initial volumes

    source->set_text(kBase + kUsb);
    auto events = poll_ok(m);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == VolumeEventKind::Mounted);
    CHECK(events[0].volume.mount_point == "/media/u/STICK");
    CHECK(events[0].volume.kind == navigator::VolumeKind::Removable);
    CHECK(m.volumes().size() == 2);

    source->set_text(kBase + kUsbRo); // remount read-only, same mount id
    events = poll_ok(m);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == VolumeEventKind::Changed);
    CHECK(events[0].volume.read_only);

    source->set_text(kBase + kUsbRemounted); // unmount + mount again: new mount id
    events = poll_ok(m);
    REQUIRE(events.size() == 2);
    CHECK(events[0].kind == VolumeEventKind::Unmounted);
    CHECK(events[0].volume.mount_id == 29);
    CHECK(events[1].kind == VolumeEventKind::Mounted);
    CHECK(events[1].volume.mount_id == 31);

    source->set_text(kBase);
    events = poll_ok(m);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == VolumeEventKind::Unmounted);
    CHECK(events[0].volume.mount_point == "/media/u/STICK");
    CHECK_FALSE(events[0].volume.read_only); // last known state
    CHECK(m.volumes().size() == 1);
}

TEST_CASE("monitor reports label changes and keeps the snapshot on error", "[volume][monitor]") {
    FakeSystem sys;
    sys.add_label("OLD", "sdb1");
    auto source = std::make_shared<FakeMountSource>(kBase + kUsb);
    auto created = navigator::create_volume_monitor(sys.query(source));
    REQUIRE(created.has_value());
    auto& m = **created;
    REQUIRE(m.volumes().size() == 2);
    CHECK(m.volumes()[1].label == "OLD");

    // No change_fd: the pipeline re-runs on every poll, so a relabel is noticed.
    sys.remove_label("OLD");
    sys.add_label("NEW", "sdb1");
    auto events = poll_ok(m);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == VolumeEventKind::Changed);
    CHECK(events[0].volume.label == "NEW");

    source->fail_next = true;
    source->set_text(kBase);
    std::vector<VolumeEvent> out;
    auto failed = m.poll(out);
    REQUIRE_FALSE(failed.has_value());
    CHECK(failed.error().code == navigator::ErrorCode::IoError);
    CHECK(out.empty());
    CHECK(m.volumes().size() == 2); // previous snapshot kept

    events = poll_ok(m);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == VolumeEventKind::Unmounted);
}

TEST_CASE("monitor wait() without change_fd is a polling interval", "[volume][monitor]") {
    FakeSystem sys;
    auto source = std::make_shared<FakeMountSource>(kBase);
    auto created = navigator::create_volume_monitor(sys.query(source));
    REQUIRE(created.has_value());
    auto& m = **created;
    CHECK(m.wait(0ms));
    CHECK(m.wait(-5ms)); // treated as 0, returns at once
    const auto start = std::chrono::steady_clock::now();
    CHECK(m.wait(30ms));
    CHECK(std::chrono::steady_clock::now() - start >= 25ms);
}

TEST_CASE("monitor creation fails when the source fails", "[volume][monitor]") {
    FakeSystem sys;
    auto source = std::make_shared<FakeMountSource>(kBase);
    source->fail_next = true;
    auto created = navigator::create_volume_monitor(sys.query(source));
    REQUIRE_FALSE(created.has_value());
    CHECK(created.error().code == navigator::ErrorCode::IoError);
}

TEST_CASE("monitor with a change_fd source diffs when the table text changes",
          "[volume][monitor]") {
    FakeSystem sys;
    const auto file = sys.tmp() / "mountinfo";
    write_file(file, kBase);
    auto source = navigator::create_proc_mount_source(file);
    REQUIRE(source.has_value());
    auto created = navigator::create_volume_monitor(sys.query(*source));
    REQUIRE(created.has_value());
    auto& m = **created;
    CHECK(m.native_handle() >= 0);
    CHECK_FALSE(m.wait(0ms)); // a regular file never signals POLLPRI
    CHECK(poll_ok(m).empty());

    write_file(file, kBase + kUsb);
    auto events = poll_ok(m);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == VolumeEventKind::Mounted);
    CHECK(poll_ok(m).empty());
}

TEST_CASE("smoke: monitor on the real mount table", "[volume][monitor][smoke]") {
    auto created = navigator::create_volume_monitor();
    REQUIRE(created.has_value());
    auto& m = **created;
    CHECK(m.native_handle() >= 0);
    CHECK_FALSE(m.volumes().empty());
    (void)m.wait(0ms);
    std::vector<VolumeEvent> out;
    CHECK(m.poll(out).has_value());
}
