// parse_mountinfo(), filesystem type sets, escape decoding.
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sys/sysmacros.h>

#include <navigator/volume.hpp>

#include "volume_internal.hpp"

using navigator::MountEntry;
using navigator::parse_mountinfo;
namespace pi = navigator::platform_internal;

TEST_CASE("parse_mountinfo reads every field", "[volume][mountinfo]") {
    const std::string text =
        "22 1 259:2 / / rw,relatime shared:1 - ext4 /dev/nvme0n1p2 rw,errors=remount-ro\n"
        "27 22 0:30 /@home /home rw,relatime shared:30 master:4 propagate_from:2 - btrfs "
        "/dev/sda2 rw,space_cache=v2\n"
        "29 22 8:17 / /media/oguz/MY\\040USB rw,nosuid - vfat /dev/sdb1 ro,uid=1000\n"
        "30 22 0:50 / /mnt/nfs rw - nfs4 server:/export rw,vers=4.2";
    const std::vector<MountEntry> e = parse_mountinfo(text);
    REQUIRE(e.size() == 4);

    CHECK(e[0].mount_id == 22);
    CHECK(e[0].parent_id == 1);
    CHECK(e[0].device_number == makedev(259, 2));
    CHECK(e[0].root == "/");
    CHECK(e[0].mount_point == "/");
    CHECK(e[0].mount_options == "rw,relatime");
    CHECK(e[0].fs_type == "ext4");
    CHECK(e[0].source == "/dev/nvme0n1p2");
    CHECK(e[0].super_options == "rw,errors=remount-ro");
    CHECK_FALSE(e[0].read_only);

    CHECK(e[1].root == "/@home"); // any number of optional fields
    CHECK(e[1].fs_type == "btrfs");
    CHECK(e[1].device_number == makedev(0, 30));

    CHECK(e[2].mount_point == "/media/oguz/MY USB"); // \040 decoded
    CHECK(e[2].read_only);                           // "ro" in super options

    CHECK(e[3].source == "server:/export"); // last line without newline
    CHECK(e[3].fs_type == "nfs4");
}

TEST_CASE("parse_mountinfo decodes octal escapes in root, mount point and source",
          "[volume][mountinfo]") {
    const auto e = parse_mountinfo(
        "40 22 0:70 /a\\011b /mnt/back\\134slash\\040x ro - fuse.sshfs us\\040er@host: rw\n");
    REQUIRE(e.size() == 1);
    CHECK(e[0].root == "/a\tb");
    CHECK(e[0].mount_point == "/mnt/back\\slash x");
    CHECK(e[0].source == "us er@host:");
    CHECK(e[0].read_only); // "ro" in mount options
}

TEST_CASE("parse_mountinfo skips malformed lines", "[volume][mountinfo]") {
    const std::string text = "\n"
                             "garbage\n"
                             "42 22 bad:1 / /x rw - ext4 /dev/x rw\n"
                             "43 22 8:1 / /y rw ext4 /dev/y rw\n"
                             "44 22 8:1 / /z rw - ext4\n"
                             "x 22 8:1 / /w rw - ext4 /dev/w rw\n"
                             "45 22 8:1 / /ok rw - ext4 /dev/ok rw\r\n";
    const auto e = parse_mountinfo(text);
    REQUIRE(e.size() == 1);
    CHECK(e[0].mount_point == "/ok");
    CHECK(e[0].super_options == "rw");
    CHECK(parse_mountinfo("").empty());
}

TEST_CASE("pseudo and network filesystem sets", "[volume][mountinfo]") {
    for (const char* t :
         {"proc",        "sysfs",          "devtmpfs",   "devpts", "tmpfs",      "ramfs",
          "cgroup",      "cgroup2",        "pstore",     "bpf",    "securityfs", "debugfs",
          "tracefs",     "configfs",       "fusectl",    "mqueue", "hugetlbfs",  "autofs",
          "binfmt_misc", "efivarfs",       "rpc_pipefs", "nsfs",   "overlay",    "squashfs",
          "fuse.portal", "fuse.gvfsd-fuse"}) {
        CHECK(navigator::is_pseudo_filesystem(t));
        CHECK_FALSE(navigator::is_network_filesystem(t));
    }
    for (const char* t : {"nfs", "nfs4", "cifs", "smb3", "smbfs", "9p", "afs", "ceph", "glusterfs",
                          "davfs", "fuse.sshfs", "fuse.rclone"}) {
        CHECK(navigator::is_network_filesystem(t));
        CHECK_FALSE(navigator::is_pseudo_filesystem(t));
    }
    for (const char* t : {"ext4", "btrfs", "xfs", "vfat", "exfat", "ntfs3", "fuse", "", "TMPFS"}) {
        CHECK_FALSE(navigator::is_pseudo_filesystem(t));
        CHECK_FALSE(navigator::is_network_filesystem(t));
    }
}

TEST_CASE("escape decoders", "[volume][mountinfo]") {
    CHECK(pi::decode_octal_escapes("a\\040b") == "a b");
    CHECK(pi::decode_octal_escapes("\\134") == "\\");
    CHECK(pi::decode_octal_escapes("trail\\04") == "trail\\04"); // incomplete: unchanged
    CHECK(pi::decode_octal_escapes("\\089") == "\\089");         // not octal
    CHECK(pi::decode_udev_escapes("MY\\x20USB") == "MY USB");
    CHECK(pi::decode_udev_escapes("a\\x2fb\\x5C") == "a/b\\");
    CHECK(pi::decode_udev_escapes("\\xZZ\\x2") == "\\xZZ\\x2");
    CHECK(pi::decode_udev_escapes("T\\xc3\\xbcrk\\xc3\\xa7e") == "T\xc3\xbcrk\xc3\xa7"
                                                                 "e");
}

TEST_CASE("path_at_or_below is component-wise", "[volume][mountinfo]") {
    CHECK(pi::path_at_or_below("/proc", "/proc"));
    CHECK(pi::path_at_or_below("/proc/1", "/proc"));
    CHECK(pi::path_at_or_below("/proc/", "/proc"));
    CHECK_FALSE(pi::path_at_or_below("/procfs", "/proc"));
    CHECK(pi::path_at_or_below("/anything", "/"));
    CHECK(pi::path_at_or_below("/", "/"));
    CHECK_FALSE(pi::path_at_or_below("/", "/proc"));
}
