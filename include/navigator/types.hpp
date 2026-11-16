// navigator/types.hpp
//
// Purpose: vocabulary types shared by every module: Path, FileId, Timestamp, Span, Arena, and the
//          single error policy (ErrorCode, Error, Result<T>, fail(), ok()).
// Owner:   contracts.
// Thread-safety: all types here are plain values with no shared state. Distinct objects may be
//                used from different threads; one object must not be mutated concurrently.
//
// Error policy (the only one in Navigator):
//   - Fallible functions return navigator::Result<T>. No exceptions cross module boundaries.
//   - Result<T> is std::expected<T, Error> when the standard library provides it
//     (__cpp_lib_expected >= 202211L), otherwise navigator::detail::Expected<T, Error>.
//     Use only the common subset documented in navigator/detail/expected.hpp.
//   - Create errors with `return navigator::fail(ErrorCode::NotFound, "msg");`
//     and void success with `return navigator::ok();` (or `return {};`).
//   - Cancellation of long-running calls uses std::stop_token (C++20); a cancelled call fails with
//     ErrorCode::Cancelled.
//   - Every translation unit of one build must be compiled with the same compiler and -std, so
//     that Result has one definition program-wide.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <version>

#if defined(__cpp_lib_expected) && __cpp_lib_expected >= 202211L
#include <expected>
#define NAVIGATOR_HAS_STD_EXPECTED 1
#else
#include <navigator/detail/expected.hpp>
#define NAVIGATOR_HAS_STD_EXPECTED 0
#endif

namespace navigator {

// Filesystem path. Plain alias: on Linux the native encoding is bytes (usually UTF-8); never
// assume a path is valid UTF-8. UI code converts for display only.
using Path = std::filesystem::path;

// Contiguous non-owning view.
template <class T, std::size_t Extent = std::dynamic_extent> using Span = std::span<T, Extent>;

// Identity of a file on a mounted filesystem (st_dev, st_ino). Zero/zero means "unknown".
struct FileId {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;

    friend constexpr bool operator==(const FileId&, const FileId&) = default;
};

// Point in time with nanosecond resolution, UTC (Unix epoch). Range is about 1677..2262, which
// covers every timestamp a Linux filesystem reports in practice; out-of-range values are clamped
// by the producer. Timestamp{} (the epoch) also means "unknown" where a field says so.
using Timestamp = std::chrono::sys_time<std::chrono::nanoseconds>;

// Bump/arena allocator for per-frame and per-listing allocations. Phase 1+: declaration only.
class Arena;

enum class ErrorCode : std::uint16_t {
    Unknown = 0,
    InvalidArgument = 1,
    NotFound = 2,
    PermissionDenied = 3,
    AlreadyExists = 4,
    IoError = 5,
    Cancelled = 6,
    Unsupported = 7,
    OutOfMemory = 8,
    PlatformError = 9,
};

// Stable lowercase identifier, for logs and tests (not for UI; translate via i18n keys).
[[nodiscard]] constexpr std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::Unknown: return "unknown";
    case ErrorCode::InvalidArgument: return "invalid_argument";
    case ErrorCode::NotFound: return "not_found";
    case ErrorCode::PermissionDenied: return "permission_denied";
    case ErrorCode::AlreadyExists: return "already_exists";
    case ErrorCode::IoError: return "io_error";
    case ErrorCode::Cancelled: return "cancelled";
    case ErrorCode::Unsupported: return "unsupported";
    case ErrorCode::OutOfMemory: return "out_of_memory";
    case ErrorCode::PlatformError: return "platform_error";
    }
    return "unknown";
}

// message: English, developer-facing detail (may include a path or errno text). Not translated.
struct Error {
    ErrorCode code = ErrorCode::Unknown;
    std::string message;
};

#if NAVIGATOR_HAS_STD_EXPECTED
template <class T> using Result = std::expected<T, Error>;
template <class E> using Unexpected = std::unexpected<E>;
#else
template <class T> using Result = detail::Expected<T, Error>;
template <class E> using Unexpected = detail::Unexpected<E>;
#endif

// Error value convertible to any Result<T>, including Result<void>.
[[nodiscard]] inline Unexpected<Error> fail(ErrorCode code, std::string message = {}) {
    return Unexpected<Error>(Error{code, std::move(message)});
}

// Successful Result<void>.
[[nodiscard]] inline Result<void> ok() noexcept { return {}; }

} // namespace navigator
