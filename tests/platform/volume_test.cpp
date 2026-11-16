// list_volumes() / volume_for() against a fake mount table, fake sysfs and fake devfs.
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <navigator/volume.hpp>

#include "fake_mounts.hpp"
#include "volume_internal.hpp"

namespace {

using navigator::VolumeInfo;
using navigator::VolumeKind;
using navigator::VolumeQuery;
using navigator::platform_test::FakeMountSource;
using navigator::platform_test::FakeSystem;

// A desktop-like mount table. Comments name what each line exercises.
const char* const kTable =
    // ext4 root on NVMe
    "22 1 259:2 / / rw,relatime shared:1 - ext4 /dev/nvme0n1p2 rw\n"
    // pseudo filesystems and system paths (filtered)
    "23 22 0:5 / /proc rw,nosuid shared:12 - proc proc rw\n"
    "24 22 0:21 / /sys rw shared:2 - sysfs sysfs rw\n"
    "25 24 0:26 / /sys/fs/cgroup rw - cgroup2 cgroup2 rw\n"
    "26 22 0:24 / /run rw - tmpfs tmpfs rw,size=100k\n"
    "44 22 259:1 / /boot/efi rw - vfat /dev/nvme0n1p1 rw\n"
    "38 22 7:0 / /snap/core/1 ro - squashfs /dev/loop0 ro\n"
    "37 22 0:60 / /tmp rw - tmpfs tmpfs rw\n"
    // btrfs subvolumes on one device with different roots (both kept)
    "27 22 0:30 /@home /home rw,relatime - btrfs /dev/sda2 rw,space_cache=v2\n"
    "28 22 0:30 /@data /data rw,relatime - btrfs /dev/sda2 rw,space_cache=v2\n"
    // USB stick under /media/<user>/LABEL with an escaped space
    "29 22 8:17 / /media/oguz/MY\\040USB rw,nosuid - vfat /dev/sdb1 rw,uid=1000\n"
    // network
    "30 22 0:50 / /mnt/nfs rw - nfs4 server:/export rw,vers=4.2\n"
    "31 22 0:51 / /mnt/smb rw - cifs //nas/share rw\n"
    // stacked: the later (topmost) xfs mount hides the ext4 one
    "32 22 8:3 / /mnt/stack rw - ext4 /dev/sda3 rw\n"
    "33 32 8:4 / /mnt/stack ro - xfs /dev/sda4 ro\n"
    // bind duplicates: same (device, root) as "/" and as "/srv" -> collapsed
    "34 22 259:2 / /mnt/rootbind rw - ext4 /dev/nvme0n1p2 rw\n"
    "35 22 259:3 /srv /srv rw - ext4 /dev/nvme0n1p3 rw\n"
    "36 22 259:3 /srv /home/oguz/srv-bind rw - ext4 /dev/nvme0n1p3 rw\n"
    // udisks mount under /run/media (kept despite /run); card reader without sysfs entry
    "39 22 8:33 / /run/media/oguz/CARD rw - exfat /dev/sdc1 rw\n"
    // sshfs (network), other FUSE (Other)
    "40 22 0:70 / /home/oguz/remote rw - fuse.sshfs user@host: rw\n"
    "41 22 0:71 / /mnt/fuse rw - fuse.something foo rw\n"
    // SD card reporting removable=1 in sysfs, mounted outside /media
    "45 22 179:1 / /mnt/sd rw - ext4 /dev/mmcblk0p1 rw\n"
    // "/snapshots" is not below "/snap"
    "46 22 259:4 / /snapshots rw - ext4 /dev/nvme0n1p4 rw\n";

struct Fixture {
    FakeSystem sys;
    std::shared_ptr<FakeMountSource> source = std::make_shared<FakeMountSource>(kTable);

    Fixture() {
        sys.add_disk("nvme0n1", "pci0000:00/0000:00:1d.0/0000:3d:00.0/nvme/nvme0", false,
                     {"nvme0n1p1", "nvme0n1p2", "nvme0n1p3", "nvme0n1p4"});
        sys.add_disk("sda", "pci0000:00/0000:00:17.0/ata1/host0/target0:0:0/0:0:0:0", false,
                     {"sda2", "sda3", "sda4"});
        // USB stick reporting removable=0 (common): R1 via the usb path component.
        sys.add_disk("sdb", "pci0000:00/0000:00:14.0/usb2/2-1/2-1:1.0/host6/target6:0:0/6:0:0:0",
                     false, {"sdb1"});
        sys.add_disk("mmcblk0", "platform/sdhci.0/mmc_host/mmc0/mmc0:0001", true, {"mmcblk0p1"});
        sys.add_label("MY\\x20USB", "sdb1");
        sys.add_uuid("1234-ABCD", "sdb1");
        sys.add_label("ROOT", "nvme0n1p2");
        sys.add_uuid("aaaa-bbbb-root", "nvme0n1p2");
        source->spaces["/"] = navigator::SpaceInfo{1000, 400, 300};
    }

    [[nodiscard]] VolumeQuery query() const { return sys.query(source); }
};

const VolumeInfo* find(const std::vector<VolumeInfo>& v, const std::string& mp) {
    auto it = std::find_if(v.begin(), v.end(),
                           [&](const VolumeInfo& i) { return i.mount_point.native() == mp; });
    return it == v.end() ? nullptr : &*it;
}

std::vector<std::string> mount_points(const std::vector<VolumeInfo>& v) {
    std::vector<std::string> out;
    out.reserve(v.size());
    for (const VolumeInfo& i : v) {
        out.push_back(i.mount_point.native());
    }
    return out;
}

} // namespace

TEST_CASE("list_volumes applies the documented pipeline", "[volume]") {
    Fixture fx;
    auto listed = navigator::list_volumes(fx.query());
    REQUIRE(listed.has_value());
    const std::vector<std::string> expected{
        "/",         "/home",      "/data",      "/media/oguz/MY USB",   "/mnt/nfs",
        "/mnt/smb",  "/mnt/stack", "/srv",       "/run/media/oguz/CARD", "/home/oguz/remote",
        "/mnt/fuse", "/mnt/sd",    "/snapshots",
    };
    CHECK(mount_points(*listed) == expected);
}

TEST_CASE("volume kinds, removable, labels and uuids", "[volume]") {
    Fixture fx;
    auto listed = navigator::list_volumes(fx.query());
    REQUIRE(listed.has_value());
    const auto& v = *listed;

    const VolumeInfo* root = find(v, "/");
    REQUIRE(root != nullptr);
    CHECK(root->kind == VolumeKind::Root);
    CHECK_FALSE(root->removable);
    CHECK(root->label == "ROOT");
    CHECK(root->uuid == std::optional<std::string>("aaaa-bbbb-root"));
    CHECK(root->device == "/dev/nvme0n1p2");
    CHECK(root->fs_type == "ext4");
    CHECK(root->mount_id == 22);
    CHECK(root->has_space);
    CHECK(root->space.total_bytes == 1000);
    CHECK(root->space.free_bytes == 400);
    CHECK(root->space.available_bytes == 300);

    const VolumeInfo* home = find(v, "/home");
    REQUIRE(home != nullptr);
    CHECK(home->kind == VolumeKind::Home);
    CHECK(home->root == "/@home");
    CHECK_FALSE(home->has_space); // fake source has no space for it

    const VolumeInfo* data = find(v, "/data");
    REQUIRE(data != nullptr);
    CHECK(data->kind == VolumeKind::Internal);
    CHECK(data->root == "/@data");
    CHECK(data->label.empty());
    CHECK_FALSE(data->uuid.has_value());

    const VolumeInfo* usb = find(v, "/media/oguz/MY USB");
    REQUIRE(usb != nullptr);
    CHECK(usb->kind == VolumeKind::Removable);
    CHECK(usb->removable);
    CHECK(usb->label == "MY USB"); // \x20 decoded
    CHECK(usb->uuid == std::optional<std::string>("1234-ABCD"));

    CHECK(find(v, "/mnt/nfs")->kind == VolumeKind::Network);
    CHECK(find(v, "/mnt/smb")->kind == VolumeKind::Network);
    CHECK(find(v, "/home/oguz/remote")->kind == VolumeKind::Network);
    CHECK(find(v, "/mnt/fuse")->kind == VolumeKind::Other);

    const VolumeInfo* stack = find(v, "/mnt/stack");
    REQUIRE(stack != nullptr);
    CHECK(stack->fs_type == "xfs"); // topmost wins
    CHECK(stack->mount_id == 33);
    CHECK(stack->read_only);
    CHECK(stack->kind == VolumeKind::Internal);
    CHECK_FALSE(stack->removable);

    const VolumeInfo* card = find(v, "/run/media/oguz/CARD");
    REQUIRE(card != nullptr);
    CHECK(card->removable); // R3, no sysfs entry for sdc
    CHECK(card->kind == VolumeKind::Removable);

    const VolumeInfo* sd = find(v, "/mnt/sd");
    REQUIRE(sd != nullptr);
    CHECK(sd->removable); // R1 via removable=1 of the parent disk
    CHECK(sd->kind == VolumeKind::Removable);

    CHECK(find(v, "/snapshots")->kind == VolumeKind::Internal);
    CHECK(find(v, "/srv")->kind == VolumeKind::Internal);
}

TEST_CASE("include_hidden keeps pseudo filesystems as Virtual but still collapses", "[volume]") {
    Fixture fx;
    VolumeQuery q = fx.query();
    q.include_hidden = true;
    q.query_space = false;
    auto listed = navigator::list_volumes(q);
    REQUIRE(listed.has_value());
    const auto& v = *listed;
    REQUIRE(find(v, "/proc") != nullptr);
    CHECK(find(v, "/proc")->kind == VolumeKind::Virtual);
    CHECK(find(v, "/tmp")->kind == VolumeKind::Virtual);
    CHECK(find(v, "/snap/core/1")->kind == VolumeKind::Virtual);
    CHECK(find(v, "/boot/efi")->kind == VolumeKind::Internal);
    CHECK(find(v, "/mnt/rootbind") == nullptr);       // bind duplicate
    CHECK(find(v, "/home/oguz/srv-bind") == nullptr); // bind duplicate
    CHECK(std::count_if(v.begin(), v.end(),
                        [](const VolumeInfo& i) { return i.mount_point == "/mnt/stack"; }) == 1);
    CHECK(fx.source->space_queries == 0); // query_space = false
    for (const VolumeInfo& i : v) {
        CHECK_FALSE(i.has_space);
    }
}

TEST_CASE("bind duplicates keep the shortest mount point, ties the first", "[volume]") {
    FakeSystem sys;
    auto source =
        std::make_shared<FakeMountSource>("22 1 8:1 / / rw - ext4 /dev/sda1 rw\n"
                                          "50 22 8:2 /x /mnt/longer/path rw - ext4 /dev/sda2 rw\n"
                                          "51 22 8:2 /x /mnt/aaa rw - ext4 /dev/sda2 rw\n"
                                          "52 22 8:2 /x /mnt/bbb rw - ext4 /dev/sda2 rw\n");
    auto listed = navigator::list_volumes(sys.query(source));
    REQUIRE(listed.has_value());
    CHECK(mount_points(*listed) == std::vector<std::string>{"/", "/mnt/aaa"});
}

TEST_CASE("removable is independent of kind and device-mapper is not traced", "[volume]") {
    FakeSystem sys;
    sys.add_disk("sdb", "pci0000:00/0000:00:14.0/usb1/1-2/1-2:1.0/host7/target7:0:0/7:0:0:0", false,
                 {"sdb2"});
    // A dm device that claims removable=1: rule R2 must not consult it.
    sys.add_disk("dm-0", "virtual/block", true, {});
    auto source = std::make_shared<FakeMountSource>(
        "22 1 8:18 / / rw - ext4 /dev/sdb2 rw\n"
        "60 22 253:0 / /mnt/dm rw - ext4 /dev/dm-0 rw\n"
        "61 22 253:1 / /mnt/lvm rw - ext4 /dev/mapper/vg-data rw\n"
        "62 22 253:2 / /media/oguz/LUKS rw - ext4 /dev/mapper/luks-1 rw\n");
    auto listed = navigator::list_volumes(sys.query(source));
    REQUIRE(listed.has_value());
    const auto& v = *listed;
    REQUIRE(v.size() == 4);
    CHECK(v[0].kind == VolumeKind::Root);
    CHECK(v[0].removable);
    CHECK_FALSE(v[1].removable);
    CHECK(v[1].kind == VolumeKind::Internal);
    CHECK_FALSE(v[2].removable);
    CHECK(v[3].removable); // R3 still applies
    CHECK(v[3].kind == VolumeKind::Removable);
}

TEST_CASE("missing sysfs and devfs trees are not errors", "[volume]") {
    auto source = std::make_shared<FakeMountSource>("22 1 8:1 / / rw - ext4 /dev/sda1 rw\n"
                                                    "23 22 8:17 / /media/u/S rw - vfat "
                                                    "/dev/sdb1 rw\n");
    VolumeQuery q;
    q.source = source;
    q.sysfs_root = "/nonexistent-navigator-sys";
    q.devfs_root = "/nonexistent-navigator-dev";
    q.home_dir = "/home/u";
    auto listed = navigator::list_volumes(q);
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 2);
    CHECK((*listed)[0].kind == VolumeKind::Root);
    CHECK((*listed)[0].label.empty());
    CHECK((*listed)[1].kind == VolumeKind::Removable);
}

TEST_CASE("Home kind follows home_dir and zfs counts as Internal", "[volume]") {
    FakeSystem sys;
    auto source =
        std::make_shared<FakeMountSource>("22 1 0:40 / / rw - zfs rpool/ROOT/ubuntu rw\n"
                                          "23 22 0:41 / /home/u rw - zfs rpool/USERDATA/u rw\n"
                                          "24 22 0:42 / /tank rw - bcachefs tank rw\n"
                                          "25 22 0:43 / /weird rw - ext4 not-a-device rw\n");
    VolumeQuery q = sys.query(source);
    q.home_dir = "/home/u/";
    auto listed = navigator::list_volumes(q);
    REQUIRE(listed.has_value());
    REQUIRE(listed->size() == 4);
    CHECK((*listed)[0].kind == VolumeKind::Root);
    CHECK((*listed)[1].kind == VolumeKind::Home);
    CHECK((*listed)[2].kind == VolumeKind::Internal);
    CHECK((*listed)[3].kind == VolumeKind::Other);
}

TEST_CASE("list_volumes propagates mount table errors", "[volume]") {
    Fixture fx;
    fx.source->fail_next = true;
    auto listed = navigator::list_volumes(fx.query());
    REQUIRE_FALSE(listed.has_value());
    CHECK(listed.error().code == navigator::ErrorCode::IoError);
}

TEST_CASE("volume_for picks the longest component-wise mount prefix", "[volume]") {
    Fixture fx;
    VolumeQuery q = fx.query();

    auto r = navigator::volume_for("/", q);
    REQUIRE(r.has_value());
    CHECK(r->mount_point == "/");
    CHECK(r->kind == VolumeKind::Root);
    CHECK(r->has_space);

    r = navigator::volume_for("/media/oguz/MY USB/docs/a.txt", q);
    REQUIRE(r.has_value());
    CHECK(r->mount_point == "/media/oguz/MY USB");
    CHECK(r->label == "MY USB");

    r = navigator::volume_for("/mnt/stackx/file", q); // not below /mnt/stack
    REQUIRE(r.has_value());
    CHECK(r->mount_point == "/");

    r = navigator::volume_for("/srv/../mnt/nfs/x/y", q); // lexically normalized
    REQUIRE(r.has_value());
    CHECK(r->mount_point == "/mnt/nfs");
    CHECK(r->kind == VolumeKind::Network);

    r = navigator::volume_for("/mnt/rootbind/x", q); // bind duplicate collapsed in step 2
    REQUIRE(r.has_value());
    CHECK(r->mount_point == "/");

    r = navigator::volume_for("/tmp/whatever", q); // hidden from listing, still found
    REQUIRE(r.has_value());
    CHECK(r->mount_point == "/tmp");
    CHECK(r->kind == VolumeKind::Virtual);

    r = navigator::volume_for("/mnt/stack/deep", q);
    REQUIRE(r.has_value());
    CHECK(r->fs_type == "xfs");

    q.query_space = false;
    r = navigator::volume_for("/", q);
    REQUIRE(r.has_value());
    CHECK_FALSE(r->has_space);
}

TEST_CASE("volume_for errors", "[volume]") {
    Fixture fx;
    auto empty = navigator::volume_for("", fx.query());
    REQUIRE_FALSE(empty.has_value());
    CHECK(empty.error().code == navigator::ErrorCode::InvalidArgument);

    FakeSystem sys;
    auto partial = std::make_shared<FakeMountSource>("50 1 8:2 / /data rw - ext4 /dev/sda2 rw\n");
    auto none = navigator::volume_for("/elsewhere", sys.query(partial));
    REQUIRE_FALSE(none.has_value());
    CHECK(none.error().code == navigator::ErrorCode::NotFound);

    fx.source->fail_next = true;
    auto broken = navigator::volume_for("/", fx.query());
    REQUIRE_FALSE(broken.has_value());
    CHECK(broken.error().code == navigator::ErrorCode::IoError);
}

TEST_CASE("volume_for resolves symlinks in the existing part of the path", "[volume]") {
    FakeSystem sys;
    const navigator::Path base = sys.tmp().path();
    std::filesystem::create_directories(base / "real");
    std::filesystem::create_directory_symlink(base / "real", base / "alias");
    const std::string mounted = (base / "real").native();
    auto source = std::make_shared<FakeMountSource>("22 1 8:1 / / rw - ext4 /dev/sda1 rw\n"
                                                    "70 22 8:9 / " +
                                                    mounted + " rw - ext4 /dev/sdz9 rw\n");
    auto r = navigator::volume_for(base / "alias" / "not-yet" / "file", sys.query(source));
    REQUIRE(r.has_value());
    CHECK(r->mount_point == mounted);
}

TEST_CASE("smoke: the real machine has a root volume", "[volume][smoke]") {
    auto listed = navigator::list_volumes();
    REQUIRE(listed.has_value());
    auto root = std::find_if(listed->begin(), listed->end(),
                             [](const VolumeInfo& v) { return v.mount_point == "/"; });
    REQUIRE(root != listed->end());
    if (!navigator::is_pseudo_filesystem(root->fs_type) &&
        !navigator::is_network_filesystem(root->fs_type)) {
        CHECK(root->kind == VolumeKind::Root);
    }
    CHECK(root->mount_id != 0);

    auto tmp = navigator::volume_for(std::filesystem::temp_directory_path());
    REQUIRE(tmp.has_value());
    CHECK_FALSE(tmp->mount_point.empty());
}

// Regression: lines from a real dual-boot Linux Mint laptop (navigator-ls --volumes).
const char* const kMintTable =
    "31 1 259:6 / / rw,relatime shared:1 - ext4 /dev/nvme0n1p6 rw,errors=remount-ro\n"
    "76 31 259:3 / /media/Windows rw,relatime shared:98 - fuseblk /dev/nvme0n1p3 "
    "rw,user_id=0,group_id=0,allow_other,blksize=4096\n"
    "78 31 259:5 / /media/Data rw,relatime shared:100 - fuseblk /dev/nvme0n1p5 "
    "rw,user_id=0,group_id=0,allow_other,blksize=4096\n"
    "576 31 0:51 / /var/lib/lxcfs rw,nosuid,nodev,relatime shared:181 - fuse.lxcfs lxcfs "
    "rw,user_id=0,group_id=0,allow_other\n"
    "3801 3775 259:6 /var/lib/waydroid/waydroid.prop "
    "/var/lib/waydroid/rootfs/vendor/waydroid.prop rw,relatime shared:1 - ext4 /dev/nvme0n1p6 "
    "rw,errors=remount-ro\n";

TEST_CASE("regression: internal NTFS under /media, lxcfs and file binds", "[volume][regression]") {
    FakeSystem sys;
    sys.add_disk("nvme0n1", "pci0000:00/0000:00:06.0/0000:01:00.0/nvme/nvme0", false,
                 {"nvme0n1p3", "nvme0n1p5", "nvme0n1p6"});
    auto source = std::make_shared<FakeMountSource>(kMintTable);
    VolumeQuery q = sys.query(source);
    q.query_space = false;

    auto listed = navigator::list_volumes(q);
    REQUIRE(listed.has_value());
    CHECK(mount_points(*listed) == std::vector<std::string>{"/", "/media/Windows", "/media/Data"});
    for (const char* mp : {"/media/Windows", "/media/Data"}) {
        const VolumeInfo* v = find(*listed, mp);
        REQUIRE(v != nullptr);
        CHECK_FALSE(v->removable); // R1 resolved nvme0n1 (removable=0, no usb): R3 not used
        CHECK(v->kind == VolumeKind::Internal);
        CHECK(v->fs_type == "fuseblk");
    }
    CHECK(navigator::is_pseudo_filesystem("fuse.lxcfs"));

    q.include_hidden = true;
    listed = navigator::list_volumes(q);
    REQUIRE(listed.has_value());
    REQUIRE(find(*listed, "/var/lib/lxcfs") != nullptr);
    CHECK(find(*listed, "/var/lib/lxcfs")->kind == VolumeKind::Virtual);
    REQUIRE(find(*listed, "/var/lib/waydroid/rootfs/vendor/waydroid.prop") != nullptr);
}

TEST_CASE("R3 still applies when R1 cannot resolve the device", "[volume][regression]") {
    FakeSystem sys; // empty sysfs: nvme0n1 unknown
    auto source = std::make_shared<FakeMountSource>(kMintTable);
    VolumeQuery q = sys.query(source);
    q.query_space = false;
    auto listed = navigator::list_volumes(q);
    REQUIRE(listed.has_value());
    REQUIRE(find(*listed, "/media/Windows") != nullptr);
    CHECK(find(*listed, "/media/Windows")->removable);
    CHECK(find(*listed, "/media/Windows")->kind == VolumeKind::Removable);
}

TEST_CASE("single-file bind mounts outside /var/lib are hidden", "[volume][regression]") {
    FakeSystem sys;
    const navigator::Path file = sys.tmp() / "bound.prop";
    navigator::platform_test::write_file(file, "x");
    const navigator::Path dir = sys.tmp() / "bound-dir";
    std::filesystem::create_directory(dir);
    auto source = std::make_shared<FakeMountSource>("22 1 8:1 / / rw - ext4 /dev/sda1 rw\n"
                                                    "90 22 8:1 /etc/x.prop " +
                                                    file.native() +
                                                    " rw - ext4 /dev/sda1 rw\n"
                                                    "91 22 8:1 /srv/d " +
                                                    dir.native() + " rw - ext4 /dev/sda1 rw\n");
    VolumeQuery q = sys.query(source);
    q.query_space = false;
    auto listed = navigator::list_volumes(q);
    REQUIRE(listed.has_value());
    CHECK(mount_points(*listed) == std::vector<std::string>{"/", dir.native()});
    q.include_hidden = true;
    listed = navigator::list_volumes(q);
    REQUIRE(listed.has_value());
    CHECK(find(*listed, file.native()) != nullptr);
}
