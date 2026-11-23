// src/ui/inspector/preview_backend.hpp
//
// Phase 6 glue between the ui and src/preview (docs/architecture.md "Phase 6"): which thumbnail
// cache root the app uses (AppConfig rules), the creation of the IPreviewService with the
// built-in providers and (optionally) the system thumbnailers, the request builder the Inspector
// and the list share, and the cache-key helpers. The only TU of the ui that names the preview
// implementation; builds without src/preview get no service (the Inspector shows "No preview
// available" and lists keep their type icons).
// Thread-safety: resolve_thumbnail_root() and make_request()/key helpers are pure; the factory
// runs once at startup on the UI thread (it lists /usr/share/thumbnailers: a handful of small
// files, like the session load).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <navigator/fs.hpp>
#include <navigator/preview.hpp>
#include <navigator/preview_service.hpp>
#include <navigator/types.hpp>

namespace navigator::ui {

// True when this build links src/preview (decoders, service).
[[nodiscard]] bool preview_available() noexcept;

struct ThumbnailRootInput {
    std::string_view thumbnail_dir; // AppConfig::thumbnail_dir
    std::string_view cache_dir;     // AppConfig::cache_dir
    bool headless = false;          // max_frames > 0
    bool enable_thumbnails = true;
    Path platform_root; // platform::thumbnail_cache_dir() (may be empty when unknown)
};
// AppConfig::thumbnail_dir, else <cache_dir>/thumbnails, else (not headless) the platform root;
// empty: no disk cache (headless without dirs, --no-thumbnails). Pure.
[[nodiscard]] Path resolve_thumbnail_root(const ThumbnailRootInput& in);
// platform::thumbnail_cache_dir() (~/.cache/thumbnails), or empty when unknown / no preview module.
[[nodiscard]] Path platform_thumbnail_root();

struct PreviewBackendOptions {
    Path thumbnail_root; // empty: no disk cache, no external thumbnailers
    bool external_thumbnailers = true;
    std::uint64_t cache_bytes = preview::kDefaultCacheBudgetBytes;
    std::function<void()> wake; // on_completion (pushes an SDL event)
};

// nullptr (with a log line) when the build has no preview module or the service failed to start.
[[nodiscard]] std::unique_ptr<preview::IPreviewService>
create_preview_backend(const PreviewBackendOptions& options);

// Request for `entry` at `path` (MIME from mime::guess_for_entry). Pure.
[[nodiscard]] preview::PreviewRequest make_request(const Path& path, const fs::DirEntry& entry,
                                                   preview::PreviewPurpose purpose,
                                                   preview::PixelSize target);

// preview::make_key / hash_value (a compatible local version without src/preview).
[[nodiscard]] preview::PreviewKey preview_key(const preview::PreviewRequest& request);
[[nodiscard]] std::size_t preview_key_hash(const preview::PreviewKey& key) noexcept;
struct UiPreviewKeyHash {
    [[nodiscard]] std::size_t operator()(const preview::PreviewKey& key) const noexcept {
        return preview_key_hash(key);
    }
};

// Thumbnails are worth asking for: image/*, video/* and page documents (is_page_mime) that are
// regular files (or links to files). A document without a system thumbnailer keeps its icon.
[[nodiscard]] bool wants_thumbnail(std::string_view mime, const fs::DirEntry& entry) noexcept;
// PDF, PostScript, EPUB, DjVu, OpenDocument / OOXML / legacy Office documents.
[[nodiscard]] bool is_page_mime(std::string_view mime) noexcept;

} // namespace navigator::ui
