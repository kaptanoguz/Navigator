// src/ui/shell/debug_volumes.hpp
//
// Development only (screenshots, demos): a fake volume table for the sidebar, selected with
// NAVIGATOR_DEBUG_VOLUMES=<dir> (see the top of shell/app.cpp). <dir> holds
//   mountinfo   /proc/self/mountinfo text (parse_mountinfo format)
//   space       one line per mount point: "<mount_point> <total> <free> <available>" (bytes)
//   sys/, dev/  sysfs / devfs roots for the removable and label rules (may be empty)
// The source never touches the real system. Not part of any contract.
// Thread-safety: the source is immutable after creation (safe from the volume worker).
#pragma once

#include <navigator/types.hpp>
#include <navigator/volume.hpp>

namespace navigator::ui {

// The query for `dir`, or an error when its mountinfo cannot be read.
[[nodiscard]] Result<VolumeQuery> debug_volume_query(const Path& dir, const Path& home);

} // namespace navigator::ui
