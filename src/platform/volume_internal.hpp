// Internal (src/platform): building blocks of the volume pipeline documented in
// <navigator/volume.hpp>, shared by list_volumes(), volume_for() and the volume monitor, and
// exposed to tests/platform. All functions are pure or only read the injected roots.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/volume.hpp>

namespace navigator::platform_internal {

// "\NNN" (three octal digits) -> byte; anything else is copied unchanged.
[[nodiscard]] std::string decode_octal_escapes(std::string_view text);

// udev "\xHH" (two hex digits) -> byte; anything else is copied unchanged.
[[nodiscard]] std::string decode_udev_escapes(std::string_view text);

// True when `path` equals `base` or lies below it, component-wise ("/procfs" is not below
// "/proc"). Both are plain absolute strings; a trailing '/' on either is ignored.
[[nodiscard]] bool path_at_or_below(std::string_view path, std::string_view base) noexcept;

// Pipeline step 2: stacked mounts (last per mount_point wins), then bind duplicates (shortest
// mount_point per (device_number, root) wins, ties: first). Keeps file order.
[[nodiscard]] std::vector<MountEntry> collapse_mounts(std::vector<MountEntry> entries);

// Pipeline step 3 predicate: true when the entry is dropped from the default listing. Calls
// stat(2) on the mount point of non-network filesystems (file bind mounts are hidden).
[[nodiscard]] bool is_hidden_mount(const MountEntry& entry) noexcept;

// Steps 1-4 on already-read mountinfo text. `source` answers query_space().
[[nodiscard]] Result<std::vector<VolumeInfo>>
build_volumes(std::string_view mountinfo_text, const VolumeQuery& query, IMountSource& source);

// query.source, or a fresh default proc source when it is null.
[[nodiscard]] Result<std::shared_ptr<IMountSource>> resolve_source(const VolumeQuery& query);

} // namespace navigator::platform_internal
