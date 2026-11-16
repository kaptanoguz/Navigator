// navigator/ops.hpp
//
// Purpose: file operation requests (copy, move, trash, delete, rename, mkdir) and their progress.
//          Undo/redo and the job queue build on these types.
// Owner:   contracts (header); implementation in src/ops (role: ops). STUB until the ops phase.
// Thread-safety: value types only. Operations will run on worker threads and report progress
//                to the UI thread; that interface is added with the ops phase.
#pragma once

#include <cstdint>
#include <vector>

#include <navigator/types.hpp>

namespace navigator {

enum class OpKind : std::uint8_t {
    Copy,
    Move,
    Trash,
    Delete, // permanent; UI must confirm
    Rename,
    MakeDirectory,
};

enum class ConflictPolicy : std::uint8_t {
    Ask,
    Skip,
    Overwrite,
    KeepBoth,
};

struct OpRequest {
    OpKind kind = OpKind::Copy;
    std::vector<Path> sources;
    Path destination; // directory for Copy/Move; new full path for Rename/MakeDirectory
    ConflictPolicy on_conflict = ConflictPolicy::Ask;
};

using OpId = std::uint64_t; // 0 = invalid

struct OpProgress {
    OpId id = 0;
    std::uint64_t bytes_done = 0;
    std::uint64_t bytes_total = 0; // 0 while still counting
    std::uint64_t items_done = 0;
    std::uint64_t items_total = 0;
    bool finished = false;
};

} // namespace navigator
