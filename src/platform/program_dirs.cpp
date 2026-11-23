// Phase 6 of <navigator/platform.hpp>: thumbnail_cache_dir(), data_dirs() and find_program().
// Environment is read on every call (no caching).
#include <algorithm>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <navigator/platform.hpp>

#include "launcher_internal.hpp"

namespace navigator::platform {

namespace {

constexpr std::string_view kDefaultDataDirs = "/usr/local/share:/usr/share";

// Lexically normal form without a trailing separator ("/usr/share/" -> "/usr/share").
Path normalized(const Path& path) {
    Path normal = path.lexically_normal();
    std::string text = normal.native();
    while (text.size() > 1 && text.back() == '/') {
        text.pop_back();
    }
    return {std::move(text)};
}

void append_unique(std::vector<Path>& out, const Path& path) {
    Path normal = normalized(path);
    if (std::find(out.begin(), out.end(), normal) == out.end()) {
        out.push_back(std::move(normal));
    }
}

} // namespace

Result<Path> thumbnail_cache_dir() {
    try {
        auto cache = user_dir(UserDir::Cache);
        if (!cache) {
            return Unexpected<Error>(std::move(cache.error()));
        }
        return *cache / "thumbnails";
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "thumbnail_cache_dir: out of memory");
    }
}

Result<std::vector<Path>> data_dirs() {
    try {
        std::vector<Path> out;
        const char* data_home = std::getenv("XDG_DATA_HOME");
        if (data_home != nullptr && data_home[0] == '/') {
            append_unique(out, Path(data_home));
        } else {
            auto home = user_dir(UserDir::Home);
            if (!home) {
                return Unexpected<Error>(std::move(home.error()));
            }
            append_unique(out, *home / ".local" / "share");
        }
        const char* env_dirs = std::getenv("XDG_DATA_DIRS");
        const std::string_view dirs = env_dirs != nullptr && env_dirs[0] != '\0'
                                          ? std::string_view(env_dirs)
                                          : kDefaultDataDirs;
        std::size_t start = 0;
        while (start <= dirs.size()) {
            std::size_t end = dirs.find(':', start);
            if (end == std::string_view::npos) {
                end = dirs.size();
            }
            const std::string_view entry = dirs.substr(start, end - start);
            if (!entry.empty() && entry.front() == '/') {
                append_unique(out, Path(std::string(entry)));
            }
            start = end + 1;
        }
        return out;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "data_dirs: out of memory");
    }
}

Result<Path> find_program(std::string_view name) {
    try {
        auto resolved = platform_internal::resolve_program(name, std::getenv("PATH"));
        if (!resolved) {
            Error error = std::move(resolved.error());
            if (error.message.starts_with("launch:")) {
                error.message.replace(0, 7, "find_program:");
            }
            return Unexpected<Error>(std::move(error));
        }
        return Path(std::move(*resolved));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "find_program: out of memory");
    }
}

} // namespace navigator::platform
