// navigator/preview_service.hpp
//
// Purpose: Phase 6 preview runtime: which optional decoders this build has, the built-in
//          providers, and IPreviewService: a bounded worker pool with visibility priorities,
//          coalescing of duplicate requests, cancellation, the Freedesktop thumbnail cache
//          (thumbnail.hpp) in front of the providers, and a memory-bounded LRU cache of results.
// Owner:   contracts (header); implementation in src/preview (role: preview), tests in
//          tests/preview (fake providers, fake IProcessRunner, temporary cache roots).
// Thread-safety: build_features() is pure. register_builtin_providers() runs during setup on
//                one thread. IPreviewService owns its worker threads; every member function is
//                internally synchronized and may be called from any thread, but the intended
//                caller is the UI thread, and poll() must be called from ONE thread (the
//                consumer). No member blocks on file I/O or waits for a job, except shutdown()
//                and the destructor. Completed results are immutable and shared
//                (std::shared_ptr<const PreviewResult>): the ui reads them on the UI thread
//                while the cache holds them, and uploads pixels to GPU textures there.
// Errors:  navigator::Result (types.hpp). Job failures are delivered in Completion::result;
//          they never throw.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <navigator/platform.hpp>
#include <navigator/preview.hpp>
#include <navigator/thumbnail.hpp>
#include <navigator/types.hpp>

namespace navigator::preview {

// =============================================================================================
// Build features and built-in providers
// =============================================================================================

// Optional libraries this build was compiled with (packaging decides; docs/building.md). Pure.
struct BuildFeatures {
    bool jpeg = false;   // libjpeg-turbo (DCT scaling)
    bool png = false;    // libpng (also required for the Freedesktop cache)
    bool webp = false;   // libwebp (static, first frame of animated)
    bool gif = false;    // first frame
    bool bmp = false;    // in-tree decoder
    bool heif = false;   // libheif (HEIC)
    bool avif = false;   // libheif with an AV1 decoder
    bool exif = false;   // EXIF parsing (libexif or in-tree)
    bool ffmpeg = false; // libavformat/libavcodec/libswscale/libavutil: video poster + metadata
    bool svg = false;    // librsvg
};
[[nodiscard]] BuildFeatures build_features() noexcept;

struct BuiltinProviderOptions {
    // External thumbnailers (thumbnail::load_thumbnailers); empty: none.
    std::vector<thumbnail::Thumbnailer> thumbnailers;
    // Runs them; null: no external thumbnailer provider is registered.
    std::shared_ptr<platform::IProcessRunner> runner;
    thumbnail::ThumbnailerLimits thumbnailer_limits;
    std::uint64_t decode_memory_bytes = kDefaultDecodeMemoryBytes;
};

// Registers, with their ids and priorities:
//   image.jpeg / image.png / image.webp / image.gif / image.bmp / image.heif   kPriorityNative
//       (only those the build has; sniffed format wins over the name)
//   image.svg       kPriorityNative when built with librsvg
//   video.ffmpeg    kPriorityNative for video/* when built with FFmpeg: poster frame (seek to
//                   ~10% of the duration, at most 30 s, first decodable key frame), VideoInfo
//   text.utf8       kPriorityGeneric for text/*, the JSON/XML/YAML/TOML/shell MIME types of
//                   mime.hpp, and application/octet-stream files up to 1 MiB (sniffed with
//                   looks_binary; binary -> PlaceholderPreview{Binary})
//   folder.listing  kPriorityNative for inode/directory (Inspector purpose only)
//   thumbnailer.<id> kPriorityExternal for each MIME of each thumbnailer, when `runner` is set;
//                   for video/* without FFmpeg it yields VideoPreview{poster, info.source None}
//   placeholder.coming_soon  kPriorityFallback for application/pdf and (without librsvg)
//                   image/svg+xml: PlaceholderPreview{ComingSoon}
//   placeholder.video        kPriorityFallback for video/* (no FFmpeg, no thumbnailer):
//                   Inspector purpose -> VideoPreview with an empty poster and info.source None
//                   (the ui shows "No preview available" and its Play button); Thumbnail
//                   purpose -> PlaceholderPreview{NoThumbnailer} (the row keeps its type icon)
// Errors: AlreadyExists (an id is already registered), OutOfMemory.
Result<void> register_builtin_providers(ProviderRegistry& registry,
                                        const BuiltinProviderOptions& options);

// =============================================================================================
// Service
// =============================================================================================

// Scheduling class; lower value runs first. Within a class the newest epoch runs first, then
// submission order (the rows the user scrolled to most recently win).
enum class RequestPriority : std::uint8_t {
    Inspector = 0, // the inspected item
    Visible = 1,   // thumbnails of rows / tiles on screen
    Prefetch = 2,  // neighbours of the inspected item, rows just off screen (features.md
                   // "Folder image prefetch for the inspector")
};

// Handle of a submitted job. value 0 = none. Identical pending keys share one ticket.
struct Ticket {
    std::uint64_t value = 0;

    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(const Ticket&, const Ticket&) = default;
};

struct Completion {
    Ticket ticket;
    PreviewKey key;
    // Success: the result (also in the memory cache). Errors: those of ProviderRegistry::produce
    // and thumbnail.hpp; Cancelled is never delivered (cancelled jobs vanish silently).
    Result<std::shared_ptr<const PreviewResult>> result;
};

inline constexpr std::uint64_t kDefaultCacheBudgetBytes = std::uint64_t{256} * 1024U * 1024U;
inline constexpr std::uint64_t kMinCacheBudgetBytes = std::uint64_t{16} * 1024U * 1024U;
inline constexpr std::size_t kDefaultMaxPending = 4096;

// Freedesktop disk cache use by the service.
struct ThumbnailCacheOptions {
    Path root;         // thumbnail cache root; EMPTY: no disk cache at all (no read, no write)
    bool read = true;  // look up existing thumbnails
    bool write = true; // write new thumbnails and failure markers
    thumbnail::Policy policy;
};

struct PreviewServiceOptions {
    std::shared_ptr<const ProviderRegistry> registry; // required
    // Worker threads; 0: clamp(hardware_concurrency / 2, 2, 6). Workers run at normal priority
    // (decodes are short and user-visible); external thumbnailers are niced by the runner.
    std::size_t worker_count = 0;
    // Memory-cache budget (AppConfig::preview_cache_bytes); values below kMinCacheBudgetBytes
    // are raised to it. The entries currently held by the ui (shared_ptr use_count > 1) are
    // never evicted early; they count against the budget.
    std::uint64_t cache_budget_bytes = kDefaultCacheBudgetBytes;
    ThumbnailCacheOptions thumbnails;
    std::size_t max_pending = kDefaultMaxPending; // oldest Prefetch, then Visible jobs dropped
    // Called on a worker thread after a completion is queued for poll(). Must be thread-safe,
    // cheap, non-blocking and must not call back into the service (the ui pushes an SDL user
    // event to wake the frame loop).
    std::function<void()> on_completion;
};

enum class InvalidateMode : std::uint8_t {
    Memory = 0,     // drop memory-cache entries and pending jobs (nav.refresh / F5, watch events)
    Regenerate = 1, // also ignore the disk cache and failure markers once for these paths, so
                    // the next Thumbnail request decodes again and overwrites ("refresh forces
                    // re-cache", features.md); used by an explicit "Refresh thumbnails" action
};

struct ServiceStats {
    std::uint64_t cache_bytes = 0;
    std::uint64_t cache_budget_bytes = 0;
    std::size_t cache_entries = 0;
    std::size_t pending = 0;
    std::size_t running = 0;
    std::uint64_t memory_hits = 0;
    std::uint64_t disk_hits = 0; // valid Freedesktop thumbnails used
    std::uint64_t produced = 0;  // provider runs that succeeded
    std::uint64_t thumbnails_written = 0;
    std::uint64_t failures_written = 0;
};

// Job pipeline (worker thread), per submitted key:
//   1. memory cache hit -> completion at once (submit() checks too, see below);
//   2. Thumbnail purpose with a disk cache: thumbnail::decide(); Skip -> Placeholder
//      NotGenerated; otherwise stat the target (follow links) for SourceInfo, then
//      lookup_thumbnail(size_for(max(target))) -> Valid: downscale to fit target, done
//      (ImageOrigin::ThumbnailCache); then has_failure() -> Placeholder KnownFailure; then
//      ReadOnly -> Placeholder NotGenerated;
//   3. ProviderRegistry::produce() with the request (Thumbnail purpose: target = the class
//      size, so the written thumbnail is reusable, then the result is downscaled to the
//      requested target before caching);
//   4. Thumbnail purpose with write enabled: write_thumbnail() on success, write_failure() on
//      the failures listed at thumbnail::write_failure;
//   5. insert into the memory cache (LRU, evicting least recently used entries over budget),
//      queue the Completion, call on_completion.
// Files whose mtime or size changed simply produce a new key; nothing is updated in place.
class IPreviewService {
public:
    IPreviewService() = default;
    IPreviewService(const IPreviewService&) = delete;
    IPreviewService& operator=(const IPreviewService&) = delete;
    IPreviewService(IPreviewService&&) = delete;
    IPreviewService& operator=(IPreviewService&&) = delete;
    // Implementations call shutdown() from their destructor.
    virtual ~IPreviewService() = default;

    // Queues a job (never blocks). Returns the ticket of an identical pending or running key
    // (coalescing: its priority is raised to the higher one, its epoch to the newer one). A
    // memory-cache hit still returns a fresh ticket and delivers the Completion on the next
    // poll() without running a job (the ui normally calls lookup() first). Returns Ticket{}
    // after shutdown() or for an invalid request (relative path).
    virtual Ticket submit(PreviewRequest request, RequestPriority priority,
                          std::uint64_t epoch) = 0;

    // Memory-cache lookup; touches the LRU on a hit. nullptr on a miss. Never blocks on I/O.
    [[nodiscard]] virtual std::shared_ptr<const PreviewResult>
    lookup(const PreviewKey& key) const = 0;

    // Cancels a pending job (removed) or a running one (its stop token fires; the result is
    // discarded). Unknown or finished tickets: no-op.
    virtual void cancel(Ticket ticket) noexcept = 0;

    // Cancels every pending or running job of `priority` whose epoch is older than `epoch`.
    // The ui bumps its epoch on scroll, navigation and tab switch, resubmits what is visible
    // (coalescing refreshes their epoch), then calls this: off-screen thumbnails are dropped.
    virtual void cancel_older(RequestPriority priority, std::uint64_t epoch) noexcept = 0;

    // Moves finished completions into `out` (appending), oldest first. Returns how many.
    virtual std::size_t poll(std::vector<Completion>& out) = 0;

    // Drops memory-cache entries and pending jobs whose path is `path` or lies below it (a
    // folder), see InvalidateMode.
    virtual void invalidate(const Path& path, InvalidateMode mode) = 0;

    // Changes the memory budget (clamped as in the options); evicts at once when lowered.
    virtual void set_cache_budget(std::uint64_t bytes) = 0;
    // Empties the memory cache (entries still held by the ui stay alive in the ui).
    virtual void clear_cache() noexcept = 0;

    [[nodiscard]] virtual ServiceStats stats() const = 0;

    // Cancels everything (running thumbnailer processes are killed by their runner), joins the
    // workers. Workers stuck in a blocking read of a hung network file are detached after
    // 2 s (they own no shared state by then). Idempotent.
    virtual void shutdown() noexcept = 0;
};

// Starts the workers. Errors: InvalidArgument (no registry), OutOfMemory, PlatformError
// (thread creation failed).
[[nodiscard]] Result<std::unique_ptr<IPreviewService>>
create_preview_service(PreviewServiceOptions options);

} // namespace navigator::preview
