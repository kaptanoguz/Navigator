// src/ui/inspector/preview_backend.cpp — the ui's glue to src/preview. See preview_backend.hpp.
#include "inspector/preview_backend.hpp"

#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <navigator/mime.hpp>

#if defined(NAVIGATOR_UI_HAVE_PREVIEW)
#include <navigator/platform.hpp>
#include <navigator/thumbnail.hpp>
#endif

namespace navigator::ui {

namespace {

void log_line(const std::string& msg) { std::fprintf(stderr, "Navigator: %s\n", msg.c_str()); }

#if !defined(NAVIGATOR_UI_HAVE_PREVIEW)
std::uint64_t mix64(std::uint64_t h, std::uint64_t v) noexcept {
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6U) + (h >> 2U);
    return h;
}
#endif

} // namespace

bool preview_available() noexcept {
#if defined(NAVIGATOR_UI_HAVE_PREVIEW)
    return true;
#else
    return false;
#endif
}

Path resolve_thumbnail_root(const ThumbnailRootInput& in) {
    if (!in.enable_thumbnails) {
        return {};
    }
    if (!in.thumbnail_dir.empty()) {
        return Path{std::string(in.thumbnail_dir)};
    }
    if (!in.cache_dir.empty()) {
        return Path(std::string(in.cache_dir)) / "thumbnails";
    }
    if (in.headless) {
        return {}; // headless rule: never the user's shared cache
    }
    return in.platform_root;
}

Path platform_thumbnail_root() {
#if defined(NAVIGATOR_UI_HAVE_PREVIEW)
    if (Result<Path> p = platform::thumbnail_cache_dir(); p) {
        return *p;
    }
#endif
    return {};
}

std::unique_ptr<preview::IPreviewService>
create_preview_backend(const PreviewBackendOptions& options) {
#if defined(NAVIGATOR_UI_HAVE_PREVIEW)
    auto registry = std::make_shared<preview::ProviderRegistry>();
    preview::BuiltinProviderOptions bo;
    if (options.external_thumbnailers && !options.thumbnail_root.empty()) {
        if (Result<std::vector<Path>> dirs = platform::data_dirs(); dirs) {
            const std::vector<Path> tdirs =
                thumbnail::thumbnailer_dirs(Span<const Path>(dirs->data(), dirs->size()));
            const thumbnail::ProgramExists exists = [](std::string_view program) {
                return platform::find_program(program).has_value();
            };
            if (Result<std::vector<thumbnail::Thumbnailer>> t = thumbnail::load_thumbnailers(
                    Span<const Path>(tdirs.data(), tdirs.size()), exists, std::stop_token{});
                t) {
                bo.thumbnailers = std::move(*t);
            }
        }
        if (!bo.thumbnailers.empty()) {
            if (Result<std::unique_ptr<platform::IProcessRunner>> runner =
                    platform::create_default_process_runner();
                runner) {
                bo.runner = std::shared_ptr<platform::IProcessRunner>(std::move(*runner));
            } else {
                log_line("no process runner (" + runner.error().message +
                         "); external thumbnailers disabled");
                bo.thumbnailers.clear();
            }
        }
    }
    if (Result<void> r = preview::register_builtin_providers(*registry, bo); !r) {
        log_line("preview providers failed: " + r.error().message);
        return nullptr;
    }
    preview::PreviewServiceOptions so;
    so.registry = registry;
    so.cache_budget_bytes = options.cache_bytes;
    so.thumbnails.root = options.thumbnail_root;
    so.on_completion = options.wake;
    Result<std::unique_ptr<preview::IPreviewService>> service =
        preview::create_preview_service(std::move(so));
    if (!service) {
        log_line("preview service failed: " + service.error().message);
        return nullptr;
    }
    return std::move(*service);
#else
    (void)options;
    log_line("built without src/preview: no previews or thumbnails");
    return nullptr;
#endif
}

preview::PreviewRequest make_request(const Path& path, const fs::DirEntry& entry,
                                     preview::PreviewPurpose purpose, preview::PixelSize target) {
    preview::PreviewRequest r;
    r.path = path;
    r.entry = entry;
    r.mime = std::string(mime::guess_for_entry(entry));
    r.purpose = purpose;
    r.target = target;
    return r;
}

preview::PreviewKey preview_key(const preview::PreviewRequest& request) {
#if defined(NAVIGATOR_UI_HAVE_PREVIEW)
    return preview::make_key(request);
#else
    preview::PreviewKey k;
    k.path = request.path;
    k.mtime = request.entry.mtime;
    k.size = request.entry.size;
    k.target = request.target;
    k.purpose = request.purpose;
    if (request.purpose == preview::PreviewPurpose::Inspector) {
        std::uint64_t h = 0;
        if (fs::is_directory_like(request.entry)) {
            h = mix64(h, request.folder.show_hidden ? 1U : 0U);
            h = mix64(h, static_cast<std::uint64_t>(request.folder.sort.key));
            h = mix64(h, request.folder.sort.descending ? 1U : 0U);
            h = mix64(h, request.folder.max_entries);
        } else {
            h = mix64(h, request.text.max_bytes);
            h = mix64(h, request.text.max_row_bytes);
            h = mix64(h, request.text.max_rows);
        }
        k.options = h;
    }
    return k;
#endif
}

std::size_t preview_key_hash(const preview::PreviewKey& key) noexcept {
#if defined(NAVIGATOR_UI_HAVE_PREVIEW)
    return preview::hash_value(key);
#else
    std::uint64_t h = std::hash<std::string>{}(key.path.native());
    h = mix64(h, static_cast<std::uint64_t>(key.mtime.time_since_epoch().count()));
    h = mix64(h, key.size);
    h = mix64(h, (std::uint64_t{key.target.width} << 32U) | key.target.height);
    h = mix64(h, static_cast<std::uint64_t>(key.purpose));
    h = mix64(h, key.options);
    return static_cast<std::size_t>(h);
#endif
}

bool wants_thumbnail(std::string_view mime, const fs::DirEntry& entry) noexcept {
    const bool file =
        entry.kind == fs::EntryKind::File ||
        (entry.kind == fs::EntryKind::Symlink && entry.target_kind == fs::EntryKind::File);
    if (!file || entry.metadata_error || entry.size == 0) {
        return false;
    }
    return mime.starts_with("image/") || mime.starts_with("video/") || is_page_mime(mime);
}

bool is_page_mime(std::string_view mime) noexcept {
    // Documents the common system thumbnailers render a page of (evince, gnome-epub, libgsf,
    // LibreOffice): previews of them are "page previews", a Navigator extra (features.md).
    return mime == "application/pdf" || mime == "application/postscript" ||
           mime == "application/epub+zip" || mime == "application/x-mobipocket-ebook" ||
           mime == "image/vnd.djvu" || mime == "application/msword" ||
           mime.starts_with("application/vnd.oasis.opendocument.") ||
           mime.starts_with("application/vnd.openxmlformats-officedocument.") ||
           mime.starts_with("application/vnd.ms-");
}

} // namespace navigator::ui
