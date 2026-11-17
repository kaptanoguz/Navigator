// Session and named-layout files (navigator/layout.hpp "Files"). POSIX I/O only; no XDG lookup
// (the caller passes the config root). The read / atomic-write / ".bad" rules live in
// document_files.cpp (shared with bookmarks.json and recents.json).
#include <algorithm>
#include <cerrno>
#include <compare>
#include <cstddef>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include <navigator/fs.hpp>
#include <navigator/layout.hpp>

#include "document_files.hpp"
#include "utf8.hpp"

namespace navigator::layout {
namespace {

constexpr std::string_view kJsonSuffix = ".json";
constexpr std::size_t kMaxLayoutNameBytes = 64;

Path layout_file(const Path& dir, std::string_view name) {
    std::string file_name(name);
    file_name.append(kJsonSuffix);
    return dir / file_name;
}

Result<void> check_named(const Path& dir, std::string_view name) {
    if (!is_valid_layout_name(name)) {
        return fail(ErrorCode::InvalidArgument, "invalid layout name");
    }
    if (dir.empty()) {
        return fail(ErrorCode::InvalidArgument, "empty layouts directory");
    }
    return ok();
}

} // namespace

Result<Session> load_session(const Path& file) {
    try {
        Result<std::string> text = core::read_document(file);
        if (!text) {
            return Unexpected<Error>(text.error());
        }
        Result<Session> session = session_from_json(*text);
        if (!session && session.error().code == ErrorCode::InvalidArgument) {
            return Unexpected<Error>(core::move_aside_bad(file, session.error().message));
        }
        return session;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while loading the session");
    }
}

Result<void> save_session(const Path& file, const Session& session) {
    try {
        if (file.empty()) {
            return fail(ErrorCode::InvalidArgument, "empty path");
        }
        Result<std::string> text = to_json(session);
        if (!text) {
            return Unexpected<Error>(text.error());
        }
        return core::write_atomically(file, *text);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while saving the session");
    }
}

bool is_valid_layout_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > kMaxLayoutNameBytes) {
        return false;
    }
    if (name.front() == '.' || name.front() == ' ' || name.back() == ' ') {
        return false;
    }
    for (const char c : name) {
        const auto b = static_cast<unsigned char>(c);
        if (b == '/' || b < 0x20 || b == 0x7F) {
            return false;
        }
    }
    return core::is_valid_utf8(name);
}

Result<void> save_named_layout(const Path& dir, std::string_view name, const Layout& layout) {
    try {
        if (Result<void> checked = check_named(dir, name); !checked) {
            return checked;
        }
        Result<std::string> text = layout_to_json(layout);
        if (!text) {
            return Unexpected<Error>(text.error());
        }
        return core::write_atomically(layout_file(dir, name), *text);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while saving a layout");
    }
}

Result<Layout> load_named_layout(const Path& dir, std::string_view name) {
    try {
        if (Result<void> checked = check_named(dir, name); !checked) {
            return Unexpected<Error>(checked.error());
        }
        Result<std::string> text = core::read_document(layout_file(dir, name));
        if (!text) {
            return Unexpected<Error>(text.error());
        }
        return layout_from_json(*text);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while loading a layout");
    }
}

Result<std::vector<std::string>> list_named_layouts(const Path& dir) {
    try {
        Result<std::vector<fs::DirEntry>> entries = fs::list_directory(dir);
        if (!entries) {
            switch (entries.error().code) {
            case ErrorCode::NotFound: return std::vector<std::string>{};
            case ErrorCode::InvalidArgument:
                return fail(ErrorCode::IoError, entries.error().message);
            default: return Unexpected<Error>(entries.error());
            }
        }
        std::vector<std::string> names;
        for (const fs::DirEntry& entry : *entries) {
            const bool regular =
                entry.kind == fs::EntryKind::File ||
                (entry.kind == fs::EntryKind::Symlink && entry.target_kind == fs::EntryKind::File);
            const std::string_view file_name = entry.name;
            if (!regular || file_name.size() <= kJsonSuffix.size() ||
                !file_name.ends_with(kJsonSuffix)) {
                continue;
            }
            const std::string_view stem =
                file_name.substr(0, file_name.size() - kJsonSuffix.size());
            if (is_valid_layout_name(stem)) {
                names.emplace_back(stem);
            }
        }
        const fs::SortOptions order;
        std::sort(names.begin(), names.end(), [&](const std::string& a, const std::string& b) {
            const std::weak_ordering c = fs::compare_names(a, b, order);
            return c != 0 ? c < 0 : a < b;
        });
        return names;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while listing layouts");
    }
}

Result<void> delete_named_layout(const Path& dir, std::string_view name) {
    try {
        if (Result<void> checked = check_named(dir, name); !checked) {
            return checked;
        }
        const Path file = layout_file(dir, name);
        if (::unlink(file.c_str()) != 0) {
            const int err = errno;
            return Unexpected<Error>(Error{
                err == ENOENT || err == ENOTDIR ? ErrorCode::NotFound : core::map_write_errno(err),
                core::errno_message("cannot delete", file, err)});
        }
        return ok();
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "out of memory while deleting a layout");
    }
}

} // namespace navigator::layout
