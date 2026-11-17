// navigator/volume.hpp
//
// Purpose: mounted volumes, removable media, space usage and mount/unmount notifications.
//          All system inputs (mount table, statvfs, /sys, /dev/disk) are injectable so tests and
//          CI never need real hardware, root, or a particular machine layout.
// Owner:   contracts (header); implementation in src/platform (role: platform). Phase 1.
// Thread-safety: parse_mountinfo(), classify helpers and VolumeQuery/VolumeInfo/MountEntry are
//                pure/plain values. list_volumes() and volume_for() are reentrant and may run
//                concurrently on any threads provided the IMountSource they use is safe for
//                that (the default source is stateless and thread-safe; a custom source shared
//                between threads must be thread-safe itself). They block on I/O, and statvfs on
//                a hung network mount can block for a long time: never call them on the UI
//                thread. An IVolumeMonitor follows the IWatcher rule (watch.hpp): one thread at a
//                time, no internal thread.
//                Phase 4 helpers (used_fraction, VolumeIdentity, identity_of, volume_matches) are
//                header-only and pure.
// Errors:  navigator::Result (types.hpp); no exceptions escape.
//
// Pipeline used by list_volumes() (documented so a fake environment produces exact results):
//   1. text = source->read_mountinfo(); entries = parse_mountinfo(text).
//   2. Stacked mounts: when several entries share a mount_point, only the last one (topmost) is
//      kept. Bind duplicates: when several entries share (device_number, root), only the one
//      with the shortest mount_point is kept (ties: first in file order). Btrfs subvolumes have
//      distinct roots ("/@", "/@home") and are all kept.
//   3. Filter (skipped when VolumeQuery::include_hidden): drop the entry when
//      is_pseudo_filesystem(fs_type), or its mount_point is "/proc", "/sys", "/dev", "/run",
//      "/snap", "/boot", "/var/lib", "/var/snap" or below one of them -- except "/run/media"
//      and below, which is kept -- or its mount_point exists and is not a directory (a single
//      file bind mount; checked with stat(2) for non-network filesystems only, a failed stat
//      keeps the entry). "/" is never dropped.
//   4. For each kept entry, fill VolumeInfo: removable (rule R), label/uuid (rule L), kind
//      (rule K), space via source->query_space() when VolumeQuery::query_space.
//   Output order: mountinfo order.
//
// Rule R (removable), device names taken as the basename of MountEntry::source when it starts
// with "/dev/" (e.g. "/dev/sdb1" -> "sdb1"); otherwise not removable by rules R1-R2:
//   R1. Disk d = name if "<sysfs>/block/<name>" exists, else the D such that
//       "<sysfs>/block/<D>/<name>" exists (partition directory, as in real sysfs).
//       When d is found, R1 decides alone: removable if "<sysfs>/block/<d>/removable" contains
//       "1", or if the canonical path of "<sysfs>/block/<d>" contains a "/usb" component prefix
//       (USB disks often report 0); otherwise not removable, and R3 is not consulted (an
//       internal NTFS partition auto-mounted under /media stays internal).
//   R2. Device-mapper/LVM/LUKS (dm-*, /dev/mapper/*) are not traced to their slaves in Phase 1;
//       for them R1 does not apply (treated as unresolved).
//   R3. Only when R1 could not resolve the device to a sysfs block device (no "/dev/" source,
//       R2, or no matching sysfs entry): removable if mount_point is below "/media/" or
//       "/run/media/" (udisks).
// Rule L (label/uuid): for each symlink in "<devfs>/disk/by-label" and "<devfs>/disk/by-uuid",
//   the link target's basename is compared with the device name; on a match the link's file
//   name (by-label: with udev "\xHH" escapes decoded) becomes label / uuid. Missing directories
//   are not an error.
// Rule K (kind), first match wins:
//   Virtual   is_pseudo_filesystem(fs_type)   (only visible with include_hidden or volume_for)
//   Network   is_network_filesystem(fs_type)
//   Root      mount_point == "/"
//   Home      mount_point == VolumeQuery::home_dir
//   Removable rule R
//   Internal  source starts with "/dev/", or fs_type is "zfs" or "bcachefs"
//   Other     anything else (e.g. other FUSE filesystems)
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/types.hpp>

namespace navigator {

// One line of /proc/<pid>/mountinfo (see proc(5)), with "\NNN" octal escapes decoded in root,
// mount_point and source.
struct MountEntry {
    std::uint32_t mount_id = 0;
    std::uint32_t parent_id = 0;
    std::uint64_t device_number = 0; // makedev(major, minor) of field 3
    std::string root;                // root of the mount within its filesystem, e.g. "/@home"
    Path mount_point;
    std::string mount_options; // per-mount options, e.g. "rw,relatime"
    std::string fs_type;       // e.g. "ext4", "fuse.sshfs"
    std::string source;        // e.g. "/dev/nvme0n1p2", "server:/export", "tmpfs"
    std::string super_options; // per-superblock options
    bool read_only = false;    // "ro" in mount_options or super_options
};

// Parses mountinfo text. Malformed lines (missing fields, no " - " separator, bad numbers) are
// skipped, never fatal; an empty input yields an empty vector. Handles any number of optional
// fields (shared:N, master:N, ...). Pure; no I/O.
[[nodiscard]] std::vector<MountEntry> parse_mountinfo(std::string_view text);

// Pseudo/virtual filesystem types that are never user volumes: proc, sysfs, devtmpfs, devpts,
// tmpfs, ramfs, cgroup, cgroup2, pstore, bpf, securityfs, debugfs, tracefs, configfs, fusectl,
// mqueue, hugetlbfs, autofs, binfmt_misc, efivarfs, rpc_pipefs, nsfs, overlay, squashfs,
// fuse.portal, fuse.gvfsd-fuse, fuse.lxcfs. (squashfs = snaps; overlay = containers; lxcfs =
// LXC/Waydroid service FUSE; fuse.gvfsd-fuse is hidden only until the GVfs phase, which will
// surface GVfs mounts itself.)
[[nodiscard]] bool is_pseudo_filesystem(std::string_view fs_type) noexcept;

// nfs, nfs4, cifs, smb3, smbfs, 9p, afs, ceph, glusterfs, davfs, fuse.sshfs, fuse.rclone.
[[nodiscard]] bool is_network_filesystem(std::string_view fs_type) noexcept;

enum class VolumeKind : std::uint8_t {
    Root = 0,      // "/"
    Home = 1,      // the user's home directory is a mount point
    Internal = 2,  // fixed local block device (ext4/btrfs/xfs/... on nvme/sata), zfs, bcachefs
    Removable = 3, // USB sticks, SD cards, optical, udisks mounts
    Network = 4,   // NFS, SMB, sshfs, ...
    Virtual = 5,   // pseudo filesystems (only with include_hidden, or from volume_for)
    Other = 6,
};

struct SpaceInfo {
    std::uint64_t total_bytes = 0;     // f_blocks * f_frsize
    std::uint64_t free_bytes = 0;      // f_bfree  * f_frsize (includes root-reserved blocks)
    std::uint64_t available_bytes = 0; // f_bavail * f_frsize (what an unprivileged user can use)
};

struct VolumeInfo {
    Path mount_point;
    std::string device;  // MountEntry::source, e.g. "/dev/nvme0n1p2"; may be a non-path
    std::string fs_type; // e.g. "ext4", "btrfs", "vfat"
    std::string label;   // filesystem label (rule L); may be empty -- the UI derives a name
    std::optional<std::string> uuid; // filesystem UUID (rule L), when known
    std::string root;                // MountEntry::root (btrfs subvolume etc.)
    std::uint32_t mount_id = 0;      // kernel mount id; changes on unmount + remount
    std::uint64_t device_number = 0; // MountEntry::device_number
    SpaceInfo space;                 // all zero when !has_space
    bool has_space = false;          // query_space requested and succeeded
    bool read_only = false;
    bool removable = false; // rule R (independent of kind: a removable "/" is still Root)
    VolumeKind kind = VolumeKind::Other;
};

// ---------------------------------------------------------------------------------------------
// Phase 4: usage and stable identity (header-only helpers, pure, callable from any thread)
// ---------------------------------------------------------------------------------------------

// Used share of a volume as df(1) computes "Use%": used / (used + available), where used =
// total - free. Root-reserved blocks therefore count neither as used nor as available, and a
// full disk reads 1.0 for an unprivileged user. Returns 0.0 when nothing is known (all zero),
// always in [0, 1]. The sidebar draws the bar from this value and the percent as
// round(100 * value).
[[nodiscard]] constexpr double used_fraction(const SpaceInfo& space) noexcept {
    const std::uint64_t used =
        space.total_bytes > space.free_bytes ? space.total_bytes - space.free_bytes : 0;
    const std::uint64_t avail = space.available_bytes;
    if (used == 0 && avail == 0) {
        return 0.0;
    }
    const double fraction =
        static_cast<double>(used) / (static_cast<double>(used) + static_cast<double>(avail));
    return fraction < 0.0 ? 0.0 : (fraction > 1.0 ? 1.0 : fraction);
}

// What a bookmark or recent entry remembers about the volume it lives on, so it can be shown
// "offline" while that volume is unmounted and found again when it returns (possibly at another
// mount point, e.g. "/media/u/STICK1" instead of "/media/u/STICK").
//
// Matching rule (volume_matches):
//   - uuid non-empty: the volume matches iff VolumeInfo::uuid == uuid AND VolumeInfo::root ==
//     root. The root is part of the identity because btrfs subvolumes ("/@", "/@home") share one
//     filesystem UUID. mount_point and device are ignored for matching (a stick may come back as
//     /dev/sdc1 at "/media/u/STICK1"); they are updated by resync (places.hpp).
//   - uuid empty (no /dev/disk/by-uuid entry: network shares, some FUSE and device-mapper
//     mounts): fallback, the volume matches iff device is non-empty AND VolumeInfo::device ==
//     device AND VolumeInfo::mount_point == mount_point. Such a volume is only recognised again
//     at the same mount point.
//   - uuid and device both empty: matches nothing (callers treat such an identity as absent).
struct VolumeIdentity {
    std::string uuid;   // VolumeInfo::uuid when known, else empty
    std::string root;   // VolumeInfo::root (subvolume root inside the filesystem), e.g. "/"
    std::string device; // VolumeInfo::device, e.g. "/dev/sdb1" or "server:/export"
    Path mount_point;   // where the volume was mounted when the identity was recorded

    friend bool operator==(const VolumeIdentity&, const VolumeIdentity&) = default;
};

[[nodiscard]] inline VolumeIdentity identity_of(const VolumeInfo& volume) {
    VolumeIdentity id;
    id.uuid = volume.uuid.value_or(std::string{});
    id.root = volume.root;
    id.device = volume.device;
    id.mount_point = volume.mount_point;
    return id;
}

[[nodiscard]] inline bool volume_matches(const VolumeIdentity& id,
                                         const VolumeInfo& volume) noexcept {
    if (!id.uuid.empty()) {
        return volume.uuid.has_value() && *volume.uuid == id.uuid && volume.root == id.root;
    }
    return !id.device.empty() && volume.device == id.device && volume.mount_point == id.mount_point;
}

// System access used by list_volumes()/volume_for()/IVolumeMonitor. Tests inject a fake.
class IMountSource {
public:
    IMountSource() = default;
    IMountSource(const IMountSource&) = delete;
    IMountSource& operator=(const IMountSource&) = delete;
    IMountSource(IMountSource&&) = delete;
    IMountSource& operator=(IMountSource&&) = delete;
    virtual ~IMountSource() = default;

    // Full current mountinfo text. Errors: IoError, PermissionDenied.
    [[nodiscard]] virtual Result<std::string> read_mountinfo() = 0;

    // Space of the filesystem mounted at `mount_point` (statvfs). May block (network mounts).
    // Errors: NotFound, PermissionDenied, IoError.
    [[nodiscard]] virtual Result<SpaceInfo> query_space(const Path& mount_point) = 0;

    // A file descriptor that signals a mount table change with POLLPRI/POLLERR (the kernel's
    // behaviour for /proc/self/mountinfo), owned by the source. -1 when unsupported: an
    // IVolumeMonitor then re-reads the mount table on every poll().
    [[nodiscard]] virtual int change_fd() const noexcept { return -1; }
};

// Default source: reads `mountinfo_path` (normally "/proc/self/mountinfo"), statvfs() for space.
// change_fd() is that file kept open (O_RDONLY | O_CLOEXEC).
// Errors: NotFound / PermissionDenied / IoError opening mountinfo_path.
[[nodiscard]] Result<std::shared_ptr<IMountSource>>
create_proc_mount_source(const Path& mountinfo_path = "/proc/self/mountinfo");

struct VolumeQuery {
    // nullptr: a default proc source is created per call (per monitor for IVolumeMonitor).
    std::shared_ptr<IMountSource> source;
    Path sysfs_root = "/sys";    // rule R; tests point this at a temporary tree
    Path devfs_root = "/dev";    // rule L; tests point this at a temporary tree with symlinks
    Path home_dir;               // rule K; empty: $HOME, else the passwd entry of the real uid
    bool query_space = true;     // call source->query_space() for each listed volume
    bool include_hidden = false; // skip pipeline step 3 (debugging)
};

// User-relevant volumes, per the pipeline above.
// Errors: those of read_mountinfo()/create_proc_mount_source(). A failed query_space() or a
// missing sysfs/devfs entry never fails the call (has_space = false, removable/label defaults).
[[nodiscard]] Result<std::vector<VolumeInfo>> list_volumes(const VolumeQuery& query = {});

// Volume containing `path`: the mount (after pipeline step 2, ignoring step 3's filter, so a
// path in /tmp yields its tmpfs with kind Virtual) whose mount_point is the longest
// component-wise prefix of the absolute, lexically normalized path with symlinks resolved for
// the existing part (std::filesystem::weakly_canonical semantics). Space is filled when
// query.query_space.
// Errors: InvalidArgument (empty path), NotFound (no mount matches, e.g. an incomplete fake
// table), plus those of list_volumes().
[[nodiscard]] Result<VolumeInfo> volume_for(const Path& path, const VolumeQuery& query = {});

enum class VolumeEventKind : std::uint8_t {
    Mounted = 0,   // a volume appeared in the (filtered) list
    Unmounted = 1, // a volume disappeared; `volume` is its last known state
    Changed = 2,   // same mount_point and mount_id, but read_only, label, uuid or device changed
                   // (e.g. remount ro). Space changes are not events.
};

struct VolumeEvent {
    VolumeEventKind kind = VolumeEventKind::Changed;
    VolumeInfo volume;
};

// Mount/unmount notifications. Default implementation: keeps the source's change_fd() and waits
// for POLLPRI | POLLERR with poll(2); on a signal (or on every poll() if change_fd() == -1) it
// re-runs the list_volumes() pipeline and diffs the new list against the previous snapshot,
// matching volumes by (mount_point, mount_id). Mounted/Unmounted for a moved or replaced mount
// may both appear in one poll(). Thread-safety: as IWatcher (see top of file).
class IVolumeMonitor {
public:
    IVolumeMonitor() = default;
    IVolumeMonitor(const IVolumeMonitor&) = delete;
    IVolumeMonitor& operator=(const IVolumeMonitor&) = delete;
    IVolumeMonitor(IVolumeMonitor&&) = delete;
    IVolumeMonitor& operator=(IVolumeMonitor&&) = delete;
    virtual ~IVolumeMonitor() = default;

    // Non-blocking. Appends events since the last poll() (or since creation) to `out` and
    // returns how many were appended. Updates volumes().
    // Errors: those of list_volumes(); the previous snapshot is kept on error.
    [[nodiscard]] virtual Result<std::size_t> poll(std::vector<VolumeEvent>& out) = 0;

    // Blocks until a change is signalled or `timeout` elapses (0: check only; < 0: no limit).
    // Returns true if poll() should be called. With a source lacking change_fd() it sleeps for
    // `timeout` (a polling interval; < 0 is treated as 0) and returns true.
    [[nodiscard]] virtual bool wait(std::chrono::milliseconds timeout) = 0;

    // POLLPRI-signalling fd for external event loops (owned; do not read or close), or -1.
    [[nodiscard]] virtual int native_handle() const noexcept = 0;

    // Snapshot as of creation or the last successful poll().
    [[nodiscard]] virtual const std::vector<VolumeInfo>& volumes() const noexcept = 0;
};

// Takes the initial snapshot synchronously (blocking I/O, as list_volumes()); no events are
// generated for volumes present at creation.
// Errors: those of list_volumes().
[[nodiscard]] Result<std::unique_ptr<IVolumeMonitor>>
create_volume_monitor(const VolumeQuery& query = {});

} // namespace navigator
