// navigator/volume.hpp
//
// Purpose: mounted volumes, removable media and space usage.
// Owner:   contracts (header); implementation in src/platform (role: platform). STUB until
//          Phase 1.
// Thread-safety: free functions are reentrant; they perform blocking I/O (call off the UI thread).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <navigator/types.hpp>

namespace navigator {

struct VolumeInfo {
    Path mount_point;
    std::string device;  // e.g. "/dev/nvme0n1p2"; empty for virtual filesystems
    std::string fs_type; // e.g. "ext4", "btrfs", "vfat"
    std::string label;   // display label, may be empty
    std::uint64_t total_bytes = 0;
    std::uint64_t free_bytes = 0; // available to the current user
    bool removable = false;
    bool read_only = false;
};

// Phase 1. User-relevant mounts (pseudo filesystems such as proc/sysfs filtered out).
[[nodiscard]] Result<std::vector<VolumeInfo>> list_volumes();

// Phase 1. Volume that contains `path`.
[[nodiscard]] Result<VolumeInfo> volume_for(const Path& path);

} // namespace navigator
