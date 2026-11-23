// Compile-only check that the Phase 6 contracts (preview values and pure helpers, the provider
// plug-in point and registry, the Freedesktop thumbnail cache and policy, system thumbnailers
// run through an injected process runner, the preview service, the Inspector session state,
// AppConfig preview fields and the Inspector CommandIds) are usable as documented. Nothing here
// is linked or run.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <navigator/commands.hpp>
#include <navigator/fs.hpp>
#include <navigator/layout.hpp>
#include <navigator/platform.hpp>
#include <navigator/preview.hpp>
#include <navigator/preview_service.hpp>
#include <navigator/thumbnail.hpp>
#include <navigator/types.hpp>
#include <navigator/ui.hpp>

namespace navigator::contracts_check {

namespace pv = navigator::preview;
namespace th = navigator::thumbnail;
namespace lay = navigator::layout;

// Stable ids that must never change.
static_assert(static_cast<std::uint32_t>(CommandId::Inspect) == 1320);
static_assert(static_cast<std::uint32_t>(CommandId::ToggleInspectorLock) == 1321);
static_assert(static_cast<std::uint32_t>(CommandId::InspectorZoomFit) == 1322);
static_assert(static_cast<std::uint32_t>(CommandId::InspectorZoom1to1) == 1323);
static_assert(static_cast<std::uint32_t>(CommandId::InspectorPlay) == 1324);
static_assert(static_cast<std::uint32_t>(CommandId::CycleInspectorPlacement) == 1325);
static_assert(static_cast<std::uint32_t>(CommandId::ToggleInspectorGrid) == 1326);
static_assert(static_cast<std::uint32_t>(CommandId::ToggleInspectorMinimap) == 1327);

// EXIF orientation vocabulary.
static_assert(pv::orientation_from_tag(6) == pv::ExifOrientation::Rotate90);
static_assert(pv::orientation_from_tag(0) == pv::ExifOrientation::Normal);
static_assert(pv::orientation_from_tag(9) == pv::ExifOrientation::Normal);
static_assert(pv::swaps_axes(pv::ExifOrientation::Transpose));
static_assert(!pv::swaps_axes(pv::ExifOrientation::Rotate180));
static_assert(pv::oriented_size({4000, 3000}, pv::ExifOrientation::Rotate270) ==
              pv::PixelSize{3000, 4000});
static_assert(pv::PixelSize{12000, 8400}.pixels() < pv::kMaxSourcePixels); // 100 MP is fine
static_assert(pv::kMaxOutputPixels <= pv::kMaxFullDecodePixels);

// Result variant order is the PreviewKind order.
static_assert(
    std::is_same_v<std::variant_alternative_t<0, pv::PreviewContent>, pv::PlaceholderPreview>);
static_assert(std::is_same_v<std::variant_alternative_t<1, pv::PreviewContent>, pv::ImagePreview>);
static_assert(std::is_same_v<std::variant_alternative_t<2, pv::PreviewContent>, pv::VideoPreview>);
static_assert(std::is_same_v<std::variant_alternative_t<3, pv::PreviewContent>, pv::TextPreview>);
static_assert(std::is_same_v<std::variant_alternative_t<4, pv::PreviewContent>, pv::FolderPreview>);
static_assert(static_cast<std::size_t>(pv::PreviewKind::Folder) == 4);

// Thumbnail size classes.
static_assert(th::pixels(th::ThumbnailSize::Large) == 256);
static_assert(th::dir_name(th::ThumbnailSize::XLarge) == "x-large");
static_assert(th::size_for(48) == th::ThumbnailSize::Normal);
static_assert(th::size_for(129) == th::ThumbnailSize::Large);
static_assert(th::size_for(5000) == th::ThumbnailSize::XXLarge);
static_assert(th::mtime_seconds(Timestamp{std::chrono::nanoseconds{1'700'000'000'999'999'999}}) ==
              1'700'000'000);
static_assert(th::Policy{} == th::Policy{false, false});

// Inspector placement.
static_assert(lay::resolve_inspector_placement(lay::InspectorPlacement::Auto, 1280, 800) ==
              lay::InspectorPlacement::Right);
static_assert(lay::resolve_inspector_placement(lay::InspectorPlacement::Auto, 800, 1200) ==
              lay::InspectorPlacement::Bottom);
static_assert(lay::resolve_inspector_placement(lay::InspectorPlacement::Bottom, 2000, 800) ==
              lay::InspectorPlacement::Bottom);
static_assert(lay::next_placement(lay::InspectorPlacement::Bottom) ==
              lay::InspectorPlacement::Auto);
static_assert(lay::InspectorState{} == lay::InspectorState{});

// Interfaces are not copyable; values are.
static_assert(!std::is_copy_constructible_v<pv::IPreviewProvider>);
static_assert(!std::is_copy_constructible_v<pv::IPreviewService>);
static_assert(!std::is_copy_constructible_v<platform::IProcessRunner>);
static_assert(std::is_copy_constructible_v<pv::PreviewRequest>);
static_assert(std::is_copy_constructible_v<pv::PreviewResult>);
static_assert(std::is_move_constructible_v<pv::ProviderRegistry>);

// A plug-in style provider: pure can_handle, reentrant produce with cancellation.
class FakeImageProvider final : public pv::IPreviewProvider {
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "fake.image"; }
    [[nodiscard]] int can_handle(std::string_view mime, const fs::DirEntry& entry,
                                 pv::PreviewPurpose purpose) const noexcept override {
        if (entry.kind != fs::EntryKind::File || purpose == pv::PreviewPurpose::Inspector) {
            return pv::kCannotHandle;
        }
        return mime.starts_with("image/") ? pv::kPriorityNative : pv::kCannotHandle;
    }
    [[nodiscard]] Result<pv::PreviewResult>
    produce(const pv::PreviewRequest& request) const override {
        if (request.stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "stopped");
        }
        const pv::PixelSize stored{4000, 3000};
        const pv::ExifOrientation orientation = pv::orientation_from_tag(6);
        pv::PixelSize target = request.target;
        if (pv::swaps_axes(orientation)) {
            target = pv::oriented_size(target, orientation); // back to stored orientation
        }
        const std::uint32_t denom = pv::select_jpeg_scale_denom(stored, target);
        Result<pv::ImageBuffer> image =
            pv::make_image({stored.width / denom, stored.height / denom});
        if (!image) {
            return fail(image.error().code, image.error().message);
        }
        Result<pv::ImageBuffer> oriented =
            pv::apply_exif_orientation(std::move(*image), orientation);
        if (!oriented) {
            return fail(oriented.error().code, oriented.error().message);
        }
        const pv::PixelSize fit = pv::fit_size(oriented->size(), request.target);
        Result<pv::ImageBuffer> small = pv::downscale(*oriented, fit);
        if (!small) {
            return fail(small.error().code, small.error().message);
        }
        pv::ImagePreview preview;
        preview.image = std::move(*small);
        preview.info.format = pv::ImageFormat::Jpeg;
        preview.info.stored = stored;
        preview.info.orientation = orientation;
        preview.info.display = pv::oriented_size(stored, orientation);
        pv::ExifInfo exif;
        exif.orientation = orientation;
        exif.make = "Canon";
        exif.taken.year = 2026;
        exif.taken.utc_offset_minutes = static_cast<std::int16_t>(180);
        exif.exposure_time = pv::Rational{1, 250};
        exif.has_gps = true;
        exif.gps_latitude = 41.0082;
        preview.info.exif = std::move(exif);
        pv::PreviewResult result;
        result.content = std::move(preview);
        result.provider_id = std::string(id());
        return result;
    }
};

std::size_t use_registry(const Path& folder, const fs::DirEntry& entry) {
    pv::ProviderRegistry registry;
    std::size_t total = 0;
    if (registry.add(std::make_unique<FakeImageProvider>())) {
        total += registry.size();
    }
    total += registry.find("fake.image") != nullptr ? 1U : 0U;
    total += registry.candidates("", entry, pv::PreviewPurpose::Thumbnail).size();

    const std::stop_source stop;
    pv::PreviewRequest request;
    request.path = folder / entry.name;
    request.entry = entry;
    request.purpose = pv::PreviewPurpose::Thumbnail;
    request.target = {96, 96};
    request.location.removable = true;
    request.stop = stop.get_token();
    if (Result<pv::PreviewResult> r = registry.produce(request)) {
        switch (r->kind()) {
        case pv::PreviewKind::Image: {
            const auto& image = std::get<pv::ImagePreview>(r->content);
            total += image.image.valid() ? image.image.pixels.size() : 0U;
            total += image.info.exif ? image.info.exif->make.size() : 0U;
            break;
        }
        case pv::PreviewKind::Video:
            total += std::get<pv::VideoPreview>(r->content).info.video_codec.size();
            break;
        case pv::PreviewKind::Text: {
            const auto& text = std::get<pv::TextPreview>(r->content);
            total += text.rows.empty() ? 0U : text.row_text(0).size();
            break;
        }
        case pv::PreviewKind::Folder:
            total += std::get<pv::FolderPreview>(r->content).entries.size();
            break;
        case pv::PreviewKind::Unsupported:
            total += std::get<pv::PlaceholderPreview>(r->content).reason ==
                             pv::PlaceholderReason::ComingSoon
                         ? 1U
                         : 0U;
            break;
        }
        total += pv::approx_bytes(*r);
    }
    const pv::PreviewKey key = pv::make_key(request);
    total += pv::PreviewKeyHash{}(key) & 1U;
    total += key == pv::make_key(request) ? 1U : 0U;
    return total;
}

// Text and format sniffing are pure.
std::size_t use_text(const std::vector<std::uint8_t>& bytes) {
    std::size_t total = 0;
    const Span<const std::uint8_t> head(
        bytes.data(), bytes.size() < pv::kBinarySniffBytes ? bytes.size() : pv::kBinarySniffBytes);
    total += pv::looks_binary(head) ? 1U : 0U;
    total += pv::to_string(pv::sniff_image_format(head)).size();
    pv::TextOptions options;
    options.max_row_bytes = 256;
    if (Result<pv::TextPreview> text =
            pv::decode_text(Span<const std::uint8_t>(bytes), bytes.size(), options)) {
        for (std::size_t row = 0; row < text->rows.size(); ++row) {
            total += text->row_text(row).size() + text->rows[row].line;
        }
        total += text->truncated ? 1U : 0U;
        total += text->encoding == pv::TextEncoding::Utf16Le ? 1U : 0U;
    }
    return total;
}

// A fake runner: tests/preview never spawns thumbnailers.
class FakeRunner final : public platform::IProcessRunner {
public:
    [[nodiscard]] Result<platform::ProcessResult> run(const platform::ProcessRequest& request,
                                                      std::stop_token stop) override {
        if (stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "stopped");
        }
        if (request.argv.empty() || request.timeout.count() <= 0) {
            return fail(ErrorCode::InvalidArgument, "bad request");
        }
        platform::ProcessResult result;
        result.exit_code = 0;
        result.sandboxed = request.sandbox;
        return result;
    }
};

std::size_t use_thumbnails(const Path& cache_home, const Path& file, const fs::DirEntry& entry,
                           const std::vector<Path>& data_dirs) {
    std::size_t total = 0;
    const Path root = th::cache_root_for(cache_home);
    Result<std::string> uri = th::file_uri(file);
    if (!uri) {
        return 0;
    }
    total += th::md5_hex(*uri).size() + th::thumbnail_file_name(*uri).size();
    total += th::thumbnail_path(root, th::ThumbnailSize::Normal, *uri).native().size();
    total += th::failure_path(root, *uri).native().size();
    total += th::is_inside_cache(file, root) ? 1U : 0U;

    const pv::SourceLocation location{false, true};
    switch (th::decide(file, entry, location, root, th::Policy{})) {
    case th::Decision::Generate: total += 1; break;
    case th::Decision::ReadOnly: total += 2; break;
    case th::Decision::Skip: return total;
    }

    th::SourceInfo source;
    source.path = file;
    source.uri = *uri;
    source.mtime = entry.mtime;
    source.size = entry.size;
    source.mime = "image/jpeg";
    const std::stop_source stop;
    if (Result<th::ThumbnailRead> read =
            th::lookup_thumbnail(root, th::size_for(96), source, stop.get_token())) {
        if (read->status == th::LookupStatus::Valid) {
            total += read->image.width + read->original.width;
        } else if (Result<bool> failed = th::has_failure(root, source); failed && !*failed) {
            Result<pv::ImageBuffer> image = pv::make_image({128, 96});
            th::WriteOptions options;
            options.original = {4000, 3000};
            if (image) {
                if (Result<Path> written = th::write_thumbnail(root, th::ThumbnailSize::Normal,
                                                               source, *image, options)) {
                    total += written->native().size();
                } else {
                    total += th::write_failure(root, source) ? 1U : 0U;
                }
            }
        }
    }

    // System thumbnailers.
    const std::vector<Path> dirs = th::thumbnailer_dirs(Span<const Path>(data_dirs));
    const th::ProgramExists exists = [](std::string_view program) {
        return platform::find_program(program).has_value();
    };
    Result<std::vector<th::Thumbnailer>> found =
        th::load_thumbnailers(Span<const Path>(dirs), exists, stop.get_token());
    if (Result<th::Thumbnailer> parsed = th::parse_thumbnailer(
            "[Thumbnailer Entry]\nTryExec=ffmpegthumbnailer\n"
            "Exec=ffmpegthumbnailer -i %i -o %o -s %s -f\nMimeType=video/mp4;video/webm;\n",
            "ffmpegthumbnailer", "/usr/share/thumbnailers/ffmpegthumbnailer.thumbnailer");
        parsed && found) {
        found->push_back(std::move(*parsed));
    }
    if (found) {
        if (const th::Thumbnailer* t =
                th::choose_thumbnailer(Span<const th::Thumbnailer>(*found), "video/mp4")) {
            th::ExecInputs inputs;
            inputs.input_path = file.native();
            inputs.input_uri = *uri;
            inputs.output_path = "/tmp/navigator-thumb-x/out.png";
            inputs.size = th::pixels(th::ThumbnailSize::Large);
            if (Result<std::vector<std::string>> argv = th::expand_exec(t->exec, inputs)) {
                total += argv->size();
            }
            total += th::split_exec(t->exec).has_value() ? 1U : 0U;
            FakeRunner runner;
            th::ThumbnailerLimits limits;
            limits.timeout = std::chrono::milliseconds(2000);
            if (Result<pv::ImageBuffer> poster = th::run_thumbnailer(
                    *t, source, th::ThumbnailSize::Large, runner, limits, stop.get_token())) {
                total += poster->height;
            }
        }
    }
    return total;
}

// A fake service for ui tests; the real one is created with the built-in providers.
class FakeService final : public pv::IPreviewService {
public:
    pv::Ticket submit(pv::PreviewRequest request, pv::RequestPriority priority,
                      std::uint64_t epoch) override {
        if (request.path.is_relative()) {
            return {};
        }
        last_priority_ = priority;
        last_epoch_ = epoch;
        return pv::Ticket{++next_};
    }
    [[nodiscard]] std::shared_ptr<const pv::PreviewResult>
    lookup(const pv::PreviewKey& key) const override {
        return key.size == 0 ? nullptr : std::make_shared<const pv::PreviewResult>();
    }
    void cancel(pv::Ticket ticket) noexcept override { last_epoch_ = ticket.value; }
    void cancel_older(pv::RequestPriority priority, std::uint64_t epoch) noexcept override {
        last_priority_ = priority;
        last_epoch_ = epoch;
    }
    std::size_t poll(std::vector<pv::Completion>& out) override {
        pv::Completion done;
        done.ticket = pv::Ticket{next_};
        done.result = std::make_shared<const pv::PreviewResult>();
        out.push_back(std::move(done));
        return 1;
    }
    void invalidate(const Path& path, pv::InvalidateMode mode) override {
        last_epoch_ = path.empty() || mode == pv::InvalidateMode::Memory ? 0U : 1U;
    }
    void set_cache_budget(std::uint64_t bytes) override { budget_ = bytes; }
    void clear_cache() noexcept override {}
    [[nodiscard]] pv::ServiceStats stats() const override {
        pv::ServiceStats s;
        s.cache_budget_bytes = budget_;
        return s;
    }
    void shutdown() noexcept override {}

private:
    std::uint64_t next_ = 0;
    std::uint64_t last_epoch_ = 0;
    std::uint64_t budget_ = pv::kDefaultCacheBudgetBytes;
    pv::RequestPriority last_priority_ = pv::RequestPriority::Visible;
};

std::size_t use_service(const AppConfig& config, const std::vector<th::Thumbnailer>& found) {
    std::size_t total = 0;
    const pv::BuildFeatures features = pv::build_features();
    total += features.ffmpeg ? 1U : 0U;

    auto registry = std::make_shared<pv::ProviderRegistry>();
    pv::BuiltinProviderOptions builtin;
    builtin.thumbnailers = found;
    if (config.enable_external_thumbnailers) {
        if (Result<std::unique_ptr<platform::IProcessRunner>> runner =
                platform::create_default_process_runner()) {
            builtin.runner = std::move(*runner);
        }
    }
    if (!pv::register_builtin_providers(*registry, builtin)) {
        return 0;
    }

    pv::PreviewServiceOptions options;
    options.registry = registry;
    options.cache_budget_bytes = config.preview_cache_bytes;
    if (config.enable_thumbnails && !config.thumbnail_dir.empty()) {
        options.thumbnails.root = Path(config.thumbnail_dir);
    }
    options.thumbnails.policy.generate_on_removable = true;
    options.on_completion = [] {};
    std::vector<std::unique_ptr<pv::IPreviewService>> services;
    if (Result<std::unique_ptr<pv::IPreviewService>> service =
            pv::create_preview_service(std::move(options))) {
        services.push_back(std::move(*service));
    }
    services.push_back(std::make_unique<FakeService>());

    for (const std::unique_ptr<pv::IPreviewService>& service : services) {
        std::uint64_t epoch = 1;
        pv::PreviewRequest request;
        request.path = "/home/user/Pictures/a.jpg";
        request.purpose = pv::PreviewPurpose::Thumbnail;
        request.target = {48, 48};
        const pv::PreviewKey key = pv::make_key(request);
        if (std::shared_ptr<const pv::PreviewResult> hit = service->lookup(key)) {
            total += hit->provider_id.size();
        } else {
            const pv::Ticket ticket = service->submit(request, pv::RequestPriority::Visible, epoch);
            total += ticket.valid() ? 1U : 0U;
        }
        ++epoch; // scrolled
        service->cancel_older(pv::RequestPriority::Visible, epoch);
        std::vector<pv::Completion> done;
        service->poll(done);
        for (const pv::Completion& c : done) {
            if (c.result) {
                total += (*c.result)->kind() == pv::PreviewKind::Image ? 1U : 0U;
            } else {
                total += c.result.error().code == ErrorCode::InvalidArgument ? 1U : 0U;
            }
        }
        service->invalidate("/home/user/Pictures", pv::InvalidateMode::Memory);
        service->set_cache_budget(pv::kMinCacheBudgetBytes);
        total += static_cast<std::size_t>(service->stats().cache_budget_bytes);
        service->shutdown();
    }
    return total;
}

// Session state and AppConfig fields.
std::size_t use_config(lay::Session session) {
    session.inspector.visible = true;
    session.inspector.placement = lay::next_placement(session.inspector.placement);
    session.inspector.show_grid = !session.inspector.show_grid;
    const lay::InspectorPlacement placement = lay::resolve_inspector_placement(
        session.inspector.placement, session.window.width, session.window.height);

    AppConfig config;
    config.thumbnail_dir = "/tmp/navigator-thumbs";
    config.preview_cache_bytes = pv::kMinCacheBudgetBytes;
    config.enable_thumbnails = false;
    config.enable_external_thumbnailers = false;
    return config.thumbnail_dir.size() + (placement == lay::InspectorPlacement::Right ? 1U : 0U) +
           (session.inspector == lay::InspectorState{} ? 0U : 1U);
}

// Platform additions.
std::size_t use_platform() {
    std::size_t total = 0;
    if (Result<Path> dir = platform::thumbnail_cache_dir()) {
        total += dir->native().size();
    }
    if (Result<std::vector<Path>> dirs = platform::data_dirs()) {
        total += dirs->size();
    }
    platform::ProcessRequest request;
    request.argv = {"ffmpegthumbnailer", "-i", "/a b.mp4", "-o", "/tmp/x/out.png", "-s", "256"};
    request.timeout = std::chrono::milliseconds(5000);
    request.max_file_bytes = th::kDefaultThumbnailerOutputBytes;
    request.sandbox = true;
    request.read_only_paths.emplace_back("/a b.mp4");
    request.writable_paths.emplace_back("/tmp/x");
    FakeRunner runner;
    const std::stop_source stop;
    if (Result<platform::ProcessResult> r = runner.run(request, stop.get_token())) {
        total += r->succeeded() ? 1U : 0U;
        total += r->stderr_text.size();
    }
    return total;
}

} // namespace navigator::contracts_check
