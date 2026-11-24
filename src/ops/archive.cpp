// Phase 10 archive browsing and extraction (navigator/archive.hpp).
// Backed by libarchive when NAVIGATOR_HAVE_LIBARCHIVE=1; clean stub otherwise.
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <sys/stat.h>

#include <navigator/archive.hpp>

#if defined(NAVIGATOR_HAVE_LIBARCHIVE) && NAVIGATOR_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

namespace navigator::archive {

namespace {

std::string lowercase_ascii(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char ch : sv) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

std::string normalize_inner(std::string_view raw) {
    std::string s(raw);
    for (char& c : s) {
        if (c == '\\') {
            c = '/';
        }
    }
    while (!s.empty() && s.front() == '/') {
        s.erase(0, 1);
    }
    while (s.size() >= 2 && s[0] == '.' && s[1] == '/') {
        s.erase(0, 2);
    }
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

// Rejects empty, absolute, or ".." path-traversal archive entries (Zip Slip guard).
bool is_safe_inner_path(std::string_view normalized) noexcept {
    if (normalized.empty() || normalized.front() == '/') {
        return false;
    }
    std::size_t pos = 0;
    while (pos < normalized.size()) {
        const std::size_t slash = normalized.find('/', pos);
        const std::size_t end = (slash == std::string_view::npos) ? normalized.size() : slash;
        const std::string_view part = normalized.substr(pos, end - pos);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        pos = slash + 1;
    }
    return true;
}

#if defined(NAVIGATOR_HAVE_LIBARCHIVE) && NAVIGATOR_HAVE_LIBARCHIVE

struct ArchiveReader {
    struct ::archive* ptr = nullptr;

    ArchiveReader() : ptr(archive_read_new()) {
        if (ptr != nullptr) {
            archive_read_support_filter_all(ptr);
            archive_read_support_format_all(ptr);
        }
    }
    ~ArchiveReader() {
        if (ptr != nullptr) {
            archive_read_free(ptr);
        }
    }
    ArchiveReader(const ArchiveReader&) = delete;
    ArchiveReader& operator=(const ArchiveReader&) = delete;
};

Result<void> copy_archive_data_to_stream(struct ::archive* ar, std::ofstream& out,
                                         const std::stop_token& stop) {
    std::array<char, 65536> buf{};
    while (true) {
        if (stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "archive: cancelled");
        }
        const la_ssize_t n = archive_read_data(ar, buf.data(), buf.size());
        if (n < 0) {
            const char* err = archive_error_string(ar);
            return fail(ErrorCode::IoError,
                        err != nullptr ? std::string(err) : std::string("archive: read error"));
        }
        if (n == 0) {
            break;
        }
        out.write(buf.data(), n);
        if (!out) {
            return fail(ErrorCode::IoError, "archive: failed to write extracted file");
        }
    }
    return ok();
}

#endif

} // namespace

bool libarchive_available() noexcept {
#if defined(NAVIGATOR_HAVE_LIBARCHIVE) && NAVIGATOR_HAVE_LIBARCHIVE
    return true;
#else
    return false;
#endif
}

bool is_archive_name(std::string_view name) noexcept {
    const std::string lower = lowercase_ascii(name);
    constexpr std::array<std::string_view, 14> kSuffixes = {
        ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".zip", ".tar", ".tgz",
        ".tbz2",   ".txz",     ".7z",     ".cbz",     ".jar", ".apk", ".rar",
    };
    for (std::string_view sfx : kSuffixes) {
        if (lower.size() > sfx.size() && lower.ends_with(sfx)) {
            return true;
        }
    }
    return false;
}

std::optional<ArchivePathRef> parse_archive_path(std::string_view raw) {
    const std::size_t sep = raw.find(kArchiveSeparator);
    if (sep == std::string_view::npos || sep == 0) {
        return std::nullopt;
    }
    ArchivePathRef ref;
    ref.archive_file = Path(std::string(raw.substr(0, sep)));
    ref.inner_path = normalize_inner(raw.substr(sep + kArchiveSeparator.size()));
    return ref;
}

Path make_archive_path(const Path& archive_file, std::string_view inner_path) {
    const std::string norm = normalize_inner(inner_path);
    std::string out = archive_file.native();
    out.append(kArchiveSeparator);
    out.append(norm);
    return {std::move(out)};
}

Result<std::vector<fs::DirEntry>> list_archive_directory(const Path& archive_file,
                                                         std::string_view inner_dir,
                                                         const std::stop_token& stop) {
#if defined(NAVIGATOR_HAVE_LIBARCHIVE) && NAVIGATOR_HAVE_LIBARCHIVE
    try {
        if (stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "archive: cancelled");
        }
        std::error_code ec;
        if (!std::filesystem::exists(archive_file, ec)) {
            return fail(ErrorCode::NotFound, "archive: file not found: " + archive_file.native());
        }

        ArchiveReader reader;
        if (reader.ptr == nullptr) {
            return fail(ErrorCode::OutOfMemory, "archive: archive_read_new failed");
        }
        if (archive_read_open_filename(reader.ptr, archive_file.c_str(), 16384) != ARCHIVE_OK) {
            const char* err = archive_error_string(reader.ptr);
            return fail(ErrorCode::IoError,
                        err != nullptr ? std::string(err) : std::string("archive: open failed"));
        }

        const std::string prefix = normalize_inner(inner_dir);
        std::unordered_map<std::string, fs::DirEntry> children;
        bool matched_dir = prefix.empty();

        struct archive_entry* entry = nullptr;
        while (archive_read_next_header(reader.ptr, &entry) == ARCHIVE_OK) {
            if (stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "archive: cancelled");
            }
            const char* raw_path = archive_entry_pathname(entry);
            if (raw_path == nullptr) {
                archive_read_data_skip(reader.ptr);
                continue;
            }
            const std::string norm = normalize_inner(raw_path);
            if (!is_safe_inner_path(norm)) {
                archive_read_data_skip(reader.ptr);
                continue;
            }

            std::string_view rel = norm;
            if (!prefix.empty()) {
                if (norm == prefix) {
                    matched_dir = true;
                    archive_read_data_skip(reader.ptr);
                    continue;
                }
                if (norm.size() <= prefix.size() + 1 || !norm.starts_with(prefix) ||
                    norm[prefix.size()] != '/') {
                    archive_read_data_skip(reader.ptr);
                    continue;
                }
                matched_dir = true;
                rel = std::string_view(norm).substr(prefix.size() + 1);
            }

            const std::size_t slash = rel.find('/');
            const bool is_descendant = (slash != std::string_view::npos);
            const std::string child_name(is_descendant ? rel.substr(0, slash) : rel);

            if (is_descendant) {
                if (!children.contains(child_name)) {
                    fs::DirEntry dir_ent;
                    dir_ent.name = child_name;
                    dir_ent.kind = fs::EntryKind::Directory;
                    dir_ent.hidden = fs::is_hidden_name(child_name);
                    dir_ent.readable = true;
                    children.emplace(child_name, std::move(dir_ent));
                }
            } else {
                fs::DirEntry ent;
                ent.name = child_name;
                const auto ftype = archive_entry_filetype(entry);
                if (ftype == AE_IFDIR) {
                    ent.kind = fs::EntryKind::Directory;
                } else if (ftype == AE_IFLNK) {
                    ent.kind = fs::EntryKind::Symlink;
                } else if (ftype == AE_IFREG) {
                    ent.kind = fs::EntryKind::File;
                    const auto sz = archive_entry_size(entry);
                    ent.size = sz > 0 ? static_cast<std::uint64_t>(sz) : 0;
                } else {
                    ent.kind = fs::EntryKind::Other;
                }
                if (archive_entry_mtime_is_set(entry) != 0) {
                    const auto sec = archive_entry_mtime(entry);
                    if (sec > 0) {
                        ent.mtime = Timestamp{std::chrono::seconds{sec}};
                    }
                }
                ent.mode = static_cast<std::uint32_t>(archive_entry_mode(entry));
                ent.hidden = fs::is_hidden_name(child_name);
                ent.readable = true;
                children[child_name] = std::move(ent);
            }
            archive_read_data_skip(reader.ptr);
        }

        if (!matched_dir) {
            return fail(ErrorCode::NotFound, "archive: directory not found inside archive");
        }

        std::vector<fs::DirEntry> out;
        out.reserve(children.size());
        for (auto& [k, v] : children) {
            out.push_back(std::move(v));
        }
        return out;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "archive: out of memory");
    }
#else
    (void)archive_file;
    (void)inner_dir;
    (void)stop;
    return fail(ErrorCode::Unsupported, "archive: built without libarchive support");
#endif
}

Result<void> extract_entry_to_file(const Path& archive_file, std::string_view inner_path,
                                   const Path& dest_file, const std::stop_token& stop) {
#if defined(NAVIGATOR_HAVE_LIBARCHIVE) && NAVIGATOR_HAVE_LIBARCHIVE
    try {
        const std::string target = normalize_inner(inner_path);
        if (!is_safe_inner_path(target)) {
            return fail(ErrorCode::InvalidArgument, "archive: invalid inner path");
        }
        ArchiveReader reader;
        if (reader.ptr == nullptr) {
            return fail(ErrorCode::OutOfMemory, "archive: archive_read_new failed");
        }
        if (archive_read_open_filename(reader.ptr, archive_file.c_str(), 16384) != ARCHIVE_OK) {
            return fail(ErrorCode::NotFound, "archive: cannot open " + archive_file.native());
        }
        struct archive_entry* entry = nullptr;
        while (archive_read_next_header(reader.ptr, &entry) == ARCHIVE_OK) {
            if (stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "archive: cancelled");
            }
            const char* raw_path = archive_entry_pathname(entry);
            if (raw_path != nullptr && normalize_inner(raw_path) == target) {
                if (archive_entry_filetype(entry) != AE_IFREG) {
                    return fail(ErrorCode::InvalidArgument, "archive: entry is not a regular file");
                }
                if (dest_file.has_parent_path()) {
                    std::error_code ec;
                    std::filesystem::create_directories(dest_file.parent_path(), ec);
                }
                std::ofstream out(dest_file, std::ios::binary | std::ios::trunc);
                if (!out.is_open()) {
                    return fail(ErrorCode::IoError,
                                "archive: cannot create destination file: " + dest_file.native());
                }
                return copy_archive_data_to_stream(reader.ptr, out, stop);
            }
            archive_read_data_skip(reader.ptr);
        }
        return fail(ErrorCode::NotFound, "archive: entry not found: " + target);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "archive: out of memory");
    }
#else
    (void)archive_file;
    (void)inner_path;
    (void)dest_file;
    (void)stop;
    return fail(ErrorCode::Unsupported, "archive: built without libarchive support");
#endif
}

Result<std::vector<Path>> extract_entries_to_dir(const Path& archive_file,
                                                 const std::vector<std::string>& inner_paths,
                                                 const Path& dest_dir,
                                                 const std::stop_token& stop) {
#if defined(NAVIGATOR_HAVE_LIBARCHIVE) && NAVIGATOR_HAVE_LIBARCHIVE
    try {
        if (dest_dir.empty()) {
            return fail(ErrorCode::InvalidArgument, "archive: destination directory is empty");
        }
        std::vector<std::string> wanted;
        wanted.reserve(inner_paths.size());
        for (const std::string& p : inner_paths) {
            const std::string norm = normalize_inner(p);
            if (!is_safe_inner_path(norm)) {
                return fail(ErrorCode::InvalidArgument, "archive: unsafe inner path: " + p);
            }
            wanted.push_back(norm);
        }

        auto matches_wanted = [&](const std::string& norm) {
            if (wanted.empty()) {
                return true;
            }
            for (const std::string& w : wanted) {
                if (norm == w ||
                    (norm.size() > w.size() && norm.starts_with(w) && norm[w.size()] == '/')) {
                    return true;
                }
            }
            return false;
        };

        ArchiveReader reader;
        if (reader.ptr == nullptr) {
            return fail(ErrorCode::OutOfMemory, "archive: archive_read_new failed");
        }
        if (archive_read_open_filename(reader.ptr, archive_file.c_str(), 16384) != ARCHIVE_OK) {
            return fail(ErrorCode::NotFound, "archive: cannot open " + archive_file.native());
        }

        std::error_code ec;
        std::filesystem::create_directories(dest_dir, ec);
        if (ec) {
            return fail(ErrorCode::IoError, "archive: cannot create destination dir");
        }

        std::vector<Path> extracted;
        struct archive_entry* entry = nullptr;
        while (archive_read_next_header(reader.ptr, &entry) == ARCHIVE_OK) {
            if (stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "archive: cancelled");
            }
            const char* raw_path = archive_entry_pathname(entry);
            if (raw_path == nullptr) {
                archive_read_data_skip(reader.ptr);
                continue;
            }
            const std::string norm = normalize_inner(raw_path);
            if (!is_safe_inner_path(norm)) {
                // Zip Slip protection: skip unsafe entries.
                archive_read_data_skip(reader.ptr);
                continue;
            }
            if (!matches_wanted(norm)) {
                archive_read_data_skip(reader.ptr);
                continue;
            }

            const Path out_path = dest_dir / norm;
            const auto ftype = archive_entry_filetype(entry);
            if (ftype == AE_IFDIR) {
                std::filesystem::create_directories(out_path, ec);
                extracted.push_back(out_path);
                archive_read_data_skip(reader.ptr);
            } else if (ftype == AE_IFREG) {
                if (out_path.has_parent_path()) {
                    std::filesystem::create_directories(out_path.parent_path(), ec);
                }
                std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
                if (!out.is_open()) {
                    return fail(ErrorCode::IoError,
                                "archive: failed to open output: " + out_path.native());
                }
                auto res = copy_archive_data_to_stream(reader.ptr, out, stop);
                if (!res) {
                    return Unexpected<Error>(std::move(res.error()));
                }
                extracted.push_back(out_path);
            } else {
                // Skip symlinks/special files during extraction to prevent symlink escape.
                archive_read_data_skip(reader.ptr);
            }
        }
        return extracted;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "archive: out of memory");
    }
#else
    (void)archive_file;
    (void)inner_paths;
    (void)dest_dir;
    (void)stop;
    return fail(ErrorCode::Unsupported, "archive: built without libarchive support");
#endif
}

} // namespace navigator::archive
