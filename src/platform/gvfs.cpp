// GVfs/GIO backend for remote and device mounts (MTP, SMB, SFTP, etc.).
// Compiled with gio-2.0 when NAVIGATOR_HAVE_GVFS=1; otherwise provides a clean no-op stub.
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/platform.hpp>

#if defined(NAVIGATOR_HAVE_GVFS) && NAVIGATOR_HAVE_GVFS
#include <gio/gio.h>
#endif

namespace navigator::platform {

namespace {

std::string lowercase_ascii(std::string_view sv) {
    std::string out;
    out.reserve(sv.size());
    for (char ch : sv) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

std::string extract_scheme(std::string_view uri) {
    const std::size_t sep = uri.find("://");
    if (sep == std::string_view::npos || sep == 0) {
        return {};
    }
    return lowercase_ascii(uri.substr(0, sep));
}

#if defined(NAVIGATOR_HAVE_GVFS) && NAVIGATOR_HAVE_GVFS

fs::EntryKind map_gio_file_type(GFileType type) noexcept {
    switch (type) {
    case G_FILE_TYPE_REGULAR: return fs::EntryKind::File;
    case G_FILE_TYPE_DIRECTORY: return fs::EntryKind::Directory;
    case G_FILE_TYPE_SYMBOLIC_LINK: return fs::EntryKind::Symlink;
    case G_FILE_TYPE_SPECIAL:
    case G_FILE_TYPE_SHORTCUT:
    case G_FILE_TYPE_MOUNTABLE: return fs::EntryKind::Other;
    case G_FILE_TYPE_UNKNOWN:
    default: return fs::EntryKind::Unknown;
    }
}

class GioGvfsBackend final : public IGvfsBackend {
public:
    Result<std::vector<GvfsMount>> list_mounts() override {
        try {
            std::vector<GvfsMount> out;
            GVolumeMonitor* monitor = g_volume_monitor_get();
            if (monitor == nullptr) {
                return out;
            }
            GList* mounts = g_volume_monitor_get_mounts(monitor);
            for (GList* l = mounts; l != nullptr; l = l->next) {
                auto* mount = static_cast<GMount*>(l->data);
                if (mount == nullptr || g_mount_is_shadowed(mount) != 0) {
                    continue;
                }
                GFile* root = g_mount_get_root(mount);
                if (root == nullptr) {
                    continue;
                }
                char* uri_c = g_file_get_uri(root);
                char* name_c = g_mount_get_name(mount);
                char* local_c = g_file_get_path(root);

                if (uri_c != nullptr) {
                    const std::string uri(uri_c);
                    const std::string scheme = extract_scheme(uri);
                    // Only surface non-local GVfs mounts (MTP, SMB, SFTP, FTP, DAV, etc.).
                    if (!scheme.empty() && scheme != "file") {
                        GvfsMount m;
                        m.id = uri;
                        m.uri = uri;
                        m.scheme = scheme;
                        m.name = name_c != nullptr ? std::string(name_c) : uri;
                        if (local_c != nullptr && local_c[0] != '\0') {
                            m.local_path = Path(local_c);
                        }
                        m.can_unmount = g_mount_can_unmount(mount) != 0;
                        out.push_back(std::move(m));
                    }
                }
                g_free(uri_c);
                g_free(name_c);
                g_free(local_c);
                g_object_unref(root);
            }
            g_list_free_full(mounts, g_object_unref);
            g_object_unref(monitor);
            return out;
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "gvfs: out of memory");
        }
    }

    Result<std::vector<fs::DirEntry>> list_uri(std::string_view uri,
                                               std::stop_token stop) override {
        try {
            if (uri.empty()) {
                return fail(ErrorCode::InvalidArgument, "gvfs: empty URI");
            }
            if (stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "gvfs: cancelled");
            }
            const std::string uri_str(uri);
            GFile* dir = g_file_new_for_uri(uri_str.c_str());
            if (dir == nullptr) {
                return fail(ErrorCode::InvalidArgument, "gvfs: invalid URI");
            }

            GError* err = nullptr;
            constexpr const char* kAttrs =
                "standard::name,standard::display-name,standard::type,standard::size,"
                "standard::is-hidden,time::modified,access::can-read";
            GFileEnumerator* enumerator = g_file_enumerate_children(
                dir, kAttrs, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, &err);
            g_object_unref(dir);
            if (enumerator == nullptr) {
                const std::string msg = err != nullptr && err->message != nullptr
                                            ? std::string(err->message)
                                            : std::string("gvfs: enumerate failed");
                const ErrorCode code =
                    (err != nullptr && err->code == G_IO_ERROR_NOT_FOUND) ? ErrorCode::NotFound
                    : (err != nullptr && err->code == G_IO_ERROR_PERMISSION_DENIED)
                        ? ErrorCode::PermissionDenied
                        : ErrorCode::IoError;
                if (err != nullptr) {
                    g_error_free(err);
                }
                return fail(code, msg);
            }

            std::vector<fs::DirEntry> entries;
            while (true) {
                if (stop.stop_requested()) {
                    g_file_enumerator_close(enumerator, nullptr, nullptr);
                    g_object_unref(enumerator);
                    return fail(ErrorCode::Cancelled, "gvfs: cancelled");
                }
                GError* next_err = nullptr;
                GFileInfo* info = g_file_enumerator_next_file(enumerator, nullptr, &next_err);
                if (info == nullptr) {
                    if (next_err != nullptr) {
                        const std::string msg = next_err->message != nullptr
                                                    ? std::string(next_err->message)
                                                    : std::string("gvfs: read error");
                        g_error_free(next_err);
                        g_file_enumerator_close(enumerator, nullptr, nullptr);
                        g_object_unref(enumerator);
                        return fail(ErrorCode::IoError, msg);
                    }
                    break;
                }
                fs::DirEntry entry;
                const char* raw_name = g_file_info_get_name(info);
                const char* disp_name = g_file_info_get_display_name(info);
                if (raw_name != nullptr && raw_name[0] != '\0') {
                    entry.name = raw_name;
                } else if (disp_name != nullptr) {
                    entry.name = disp_name;
                }
                entry.kind = map_gio_file_type(g_file_info_get_file_type(info));
                if (entry.kind == fs::EntryKind::File) {
                    const goffset sz = g_file_info_get_size(info);
                    entry.size = sz > 0 ? static_cast<std::uint64_t>(sz) : 0;
                }
                const guint64 mtime_sec =
                    g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
                if (mtime_sec > 0) {
                    entry.mtime = Timestamp{std::chrono::seconds{mtime_sec}};
                }
                entry.hidden =
                    g_file_info_get_is_hidden(info) != 0 || fs::is_hidden_name(entry.name);
                entry.readable =
                    !g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ) ||
                    g_file_info_get_attribute_boolean(info, G_FILE_ATTRIBUTE_ACCESS_CAN_READ) != 0;
                g_object_unref(info);
                if (!entry.name.empty() && entry.name != "." && entry.name != "..") {
                    entries.push_back(std::move(entry));
                }
            }
            g_file_enumerator_close(enumerator, nullptr, nullptr);
            g_object_unref(enumerator);
            return entries;
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::OutOfMemory, "gvfs: out of memory");
        }
    }

    Result<Path> local_path_for_uri(std::string_view uri) override {
        if (uri.empty()) {
            return fail(ErrorCode::InvalidArgument, "gvfs: empty URI");
        }
        const std::string uri_str(uri);
        GFile* file = g_file_new_for_uri(uri_str.c_str());
        if (file == nullptr) {
            return fail(ErrorCode::InvalidArgument, "gvfs: invalid URI");
        }
        char* local = g_file_get_path(file);
        g_object_unref(file);
        if (local == nullptr || local[0] == '\0') {
            g_free(local);
            return fail(ErrorCode::NotFound, "gvfs: no local FUSE mount path for URI");
        }
        Path p(local);
        g_free(local);
        return p;
    }

    Result<void> unmount(std::string_view uri) override {
        if (uri.empty()) {
            return fail(ErrorCode::InvalidArgument, "gvfs: empty URI");
        }
        const std::string uri_str(uri);
        GFile* file = g_file_new_for_uri(uri_str.c_str());
        if (file == nullptr) {
            return fail(ErrorCode::InvalidArgument, "gvfs: invalid URI");
        }
        GError* err = nullptr;
        GMount* mount = g_file_find_enclosing_mount(file, nullptr, &err);
        g_object_unref(file);
        if (mount == nullptr) {
            const std::string msg = err != nullptr && err->message != nullptr
                                        ? std::string(err->message)
                                        : std::string("gvfs: mount not found");
            if (err != nullptr) {
                g_error_free(err);
            }
            return fail(ErrorCode::NotFound, msg);
        }
        // Async unmount with NULL callback (fire-and-forget); GIO signals mount-removed.
        g_mount_unmount_with_operation(mount, G_MOUNT_UNMOUNT_NONE, nullptr, nullptr, nullptr,
                                       nullptr);
        g_object_unref(mount);
        return ok();
    }
};

#else

class StubGvfsBackend final : public IGvfsBackend {
public:
    Result<std::vector<GvfsMount>> list_mounts() override { return std::vector<GvfsMount>{}; }

    Result<std::vector<fs::DirEntry>> list_uri(std::string_view /*uri*/,
                                               std::stop_token /*stop*/) override {
        return fail(ErrorCode::Unsupported, "gvfs: built without GIO/GVfs support");
    }

    Result<Path> local_path_for_uri(std::string_view /*uri*/) override {
        return fail(ErrorCode::Unsupported, "gvfs: built without GIO/GVfs support");
    }

    Result<void> unmount(std::string_view /*uri*/) override {
        return fail(ErrorCode::Unsupported, "gvfs: built without GIO/GVfs support");
    }
};

#endif

} // namespace

bool gvfs_available() noexcept {
#if defined(NAVIGATOR_HAVE_GVFS) && NAVIGATOR_HAVE_GVFS
    return true;
#else
    return false;
#endif
}

bool is_gvfs_uri(std::string_view raw) noexcept {
    const std::string scheme = extract_scheme(raw);
    if (scheme.empty()) {
        return false;
    }
    constexpr std::array<std::string_view, 9> kSchemes = {
        "smb", "sftp", "mtp", "ftp", "ftps", "dav", "davs", "afc", "gphoto2",
    };
    return std::ranges::find(kSchemes, scheme) != kSchemes.end();
}

Result<std::unique_ptr<IGvfsBackend>> create_gvfs_backend() {
    try {
#if defined(NAVIGATOR_HAVE_GVFS) && NAVIGATOR_HAVE_GVFS
        return std::make_unique<GioGvfsBackend>();
#else
        return std::make_unique<StubGvfsBackend>();
#endif
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "create_gvfs_backend: out of memory");
    }
}

} // namespace navigator::platform
