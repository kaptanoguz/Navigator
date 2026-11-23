// src/preview/providers.cpp: the built-in providers, build_features() and
// register_builtin_providers() (ids and priorities as documented in preview_service.hpp).
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <new>
#include <utility>

#include <navigator/preview_service.hpp>

#include "internal.hpp"

namespace navigator::preview {

namespace {

constexpr std::uint64_t kOctetTextLimit = std::uint64_t{1} << 20U;

[[nodiscard]] bool starts_with(std::string_view s, std::string_view prefix) noexcept {
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] bool is_regular_like(const fs::DirEntry& e) noexcept {
    return e.kind == fs::EntryKind::File || e.kind == fs::EntryKind::Unknown ||
           (e.kind == fs::EntryKind::Symlink && e.target_kind == fs::EntryKind::File);
}

[[nodiscard]] detail::DecodeParams params_for(const PreviewRequest& r, std::uint64_t budget) {
    detail::DecodeParams p;
    p.target = r.target;
    p.purpose = r.purpose;
    p.stop = r.stop;
    p.memory_budget = budget;
    return p;
}

template <class F> Result<PreviewResult> guarded(F&& f) noexcept {
    try {
        return std::forward<F>(f)();
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "provider: allocation failed");
    } catch (...) {
        return fail(ErrorCode::Unknown, "provider: unexpected exception");
    }
}

[[nodiscard]] Result<PreviewResult> wrap(Result<ImagePreview> r) {
    if (!r) {
        return Unexpected<Error>(r.error());
    }
    PreviewResult out;
    out.content = std::move(*r);
    return out;
}

// ---------------------------------------------------------------------------------------------

using FileDecoder = Result<ImagePreview> (*)(const Path&, const detail::DecodeParams&);

class ImageProvider final : public IPreviewProvider {
public:
    ImageProvider(std::string id, std::vector<std::string> mimes, std::uint64_t budget,
                  FileDecoder unknown_fallback = nullptr)
        : id_(std::move(id)), mimes_(std::move(mimes)), budget_(budget),
          unknown_fallback_(unknown_fallback) {}

    [[nodiscard]] std::string_view id() const noexcept override { return id_; }

    [[nodiscard]] int can_handle(std::string_view mime, const fs::DirEntry& entry,
                                 PreviewPurpose /*purpose*/) const noexcept override {
        if (!is_regular_like(entry)) {
            return kCannotHandle;
        }
        return std::find(mimes_.begin(), mimes_.end(), mime) != mimes_.end() ? kPriorityNative
                                                                             : kCannotHandle;
    }

    [[nodiscard]] Result<PreviewResult> produce(const PreviewRequest& request) const override {
        return guarded([&]() -> Result<PreviewResult> {
            const detail::DecodeParams p = params_for(request, budget_);
            auto r = detail::decode_image_file(request.path, p);
            if (!r && r.error().code == ErrorCode::Unsupported && unknown_fallback_ != nullptr &&
                detail::sniff_file(request.path) == ImageFormat::Unknown) {
                r = unknown_fallback_(request.path, p);
            }
            return wrap(std::move(r));
        });
    }

private:
    std::string id_;
    std::vector<std::string> mimes_;
    std::uint64_t budget_;
    FileDecoder unknown_fallback_;
};

class VideoProvider final : public IPreviewProvider {
public:
    explicit VideoProvider(std::uint64_t budget) : budget_(budget) {}
    [[nodiscard]] std::string_view id() const noexcept override { return "video.ffmpeg"; }
    [[nodiscard]] int can_handle(std::string_view mime, const fs::DirEntry& entry,
                                 PreviewPurpose /*purpose*/) const noexcept override {
        return is_regular_like(entry) && starts_with(mime, "video/") ? kPriorityNative
                                                                     : kCannotHandle;
    }
    [[nodiscard]] Result<PreviewResult> produce(const PreviewRequest& request) const override {
        return guarded([&]() -> Result<PreviewResult> {
            auto r = detail::decode_video_file(request.path, params_for(request, budget_));
            if (!r) {
                return Unexpected<Error>(r.error());
            }
            PreviewResult out;
            out.content = std::move(*r);
            return out;
        });
    }

private:
    std::uint64_t budget_;
};

class TextProvider final : public IPreviewProvider {
public:
    explicit TextProvider(std::uint64_t budget) : budget_(budget) {}
    [[nodiscard]] std::string_view id() const noexcept override { return "text.utf8"; }
    [[nodiscard]] int can_handle(std::string_view mime, const fs::DirEntry& entry,
                                 PreviewPurpose purpose) const noexcept override {
        if (purpose != PreviewPurpose::Inspector || !is_regular_like(entry)) {
            return kCannotHandle;
        }
        static constexpr std::string_view kTextApps[] = {"application/json", "application/xml",
                                                         "application/yaml", "application/toml",
                                                         "application/x-shellscript"};
        if (starts_with(mime, "text/") ||
            std::find(std::begin(kTextApps), std::end(kTextApps), mime) != std::end(kTextApps)) {
            return kPriorityGeneric;
        }
        if (mime == "application/octet-stream" && entry.size <= kOctetTextLimit) {
            return kPriorityGeneric;
        }
        return kCannotHandle;
    }
    [[nodiscard]] Result<PreviewResult> produce(const PreviewRequest& request) const override {
        return guarded([&]() -> Result<PreviewResult> {
            std::uint64_t size = 0;
            auto fd = detail::open_regular(request.path, &size);
            if (!fd) {
                return Unexpected<Error>(fd.error());
            }
            const std::uint64_t cap = request.text.max_bytes;
            auto bytes = detail::read_prefix(fd->get(), cap, request.stop, request.path);
            if (!bytes) {
                return Unexpected<Error>(bytes.error());
            }
            size = std::max<std::uint64_t>(size, bytes->size());
            auto text = decode_text(Span<const std::uint8_t>(bytes->data(), bytes->size()), size,
                                    request.text);
            PreviewResult out;
            if (text) {
                out.content = std::move(*text);
                return out;
            }
            if (text.error().code != ErrorCode::Unsupported) {
                return Unexpected<Error>(text.error());
            }
            // Binary: an extensionless image is still shown (sniffed content wins).
            const ImageFormat f = sniff_image_format(
                Span<const std::uint8_t>(bytes->data(), std::min<std::size_t>(bytes->size(), 256)));
            if (f != ImageFormat::Unknown) {
                auto img = detail::decode_image_file(request.path, params_for(request, budget_));
                if (img) {
                    out.content = std::move(*img);
                    return out;
                }
                if (img.error().code == ErrorCode::Cancelled) {
                    return Unexpected<Error>(img.error());
                }
            }
            out.content = PlaceholderPreview{PlaceholderReason::Binary, "binary content"};
            return out;
        });
    }

private:
    std::uint64_t budget_;
};

class FolderProvider final : public IPreviewProvider {
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "folder.listing"; }
    [[nodiscard]] int can_handle(std::string_view mime, const fs::DirEntry& entry,
                                 PreviewPurpose purpose) const noexcept override {
        if (purpose != PreviewPurpose::Inspector) {
            return kCannotHandle;
        }
        return mime == "inode/directory" || fs::is_directory_like(entry) ? kPriorityNative
                                                                         : kCannotHandle;
    }
    [[nodiscard]] Result<PreviewResult> produce(const PreviewRequest& request) const override {
        return guarded([&]() -> Result<PreviewResult> {
            fs::ListOptions lo;
            lo.show_hidden = true;
            auto listed = fs::list_directory(request.path, lo, request.stop);
            if (!listed) {
                return Unexpected<Error>(listed.error());
            }
            FolderPreview fp;
            std::vector<fs::DirEntry> shown;
            shown.reserve(listed->size());
            for (fs::DirEntry& e : *listed) {
                if (e.hidden && !request.folder.show_hidden) {
                    ++fp.hidden;
                    continue;
                }
                if (fs::is_directory_like(e)) {
                    ++fp.folders;
                } else {
                    ++fp.files;
                }
                shown.push_back(std::move(e));
            }
            if (request.stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "cancelled");
            }
            fp.total = shown.size();
            fs::sort_entries(Span<fs::DirEntry>(shown.data(), shown.size()), request.folder.sort);
            if (shown.size() > request.folder.max_entries) {
                shown.resize(request.folder.max_entries);
            }
            shown.shrink_to_fit();
            fp.entries = std::move(shown);
            fp.truncated = fp.total > fp.entries.size();
            PreviewResult out;
            out.content = std::move(fp);
            return out;
        });
    }
};

class ComingSoonProvider final : public IPreviewProvider {
public:
    explicit ComingSoonProvider(bool svg) : svg_(svg) {}
    [[nodiscard]] std::string_view id() const noexcept override {
        return "placeholder.coming_soon";
    }
    [[nodiscard]] int can_handle(std::string_view mime, const fs::DirEntry& /*entry*/,
                                 PreviewPurpose /*purpose*/) const noexcept override {
        if (mime == "application/pdf" || (svg_ && mime == "image/svg+xml")) {
            return kPriorityFallback;
        }
        return kCannotHandle;
    }
    [[nodiscard]] Result<PreviewResult> produce(const PreviewRequest& request) const override {
        return guarded([&]() -> Result<PreviewResult> {
            PreviewResult out;
            out.content = PlaceholderPreview{PlaceholderReason::ComingSoon,
                                             "no decoder yet for " + request.mime};
            return out;
        });
    }

private:
    bool svg_;
};

class VideoPlaceholderProvider final : public IPreviewProvider {
public:
    [[nodiscard]] std::string_view id() const noexcept override { return "placeholder.video"; }
    [[nodiscard]] int can_handle(std::string_view mime, const fs::DirEntry& /*entry*/,
                                 PreviewPurpose /*purpose*/) const noexcept override {
        return starts_with(mime, "video/") ? kPriorityFallback : kCannotHandle;
    }
    [[nodiscard]] Result<PreviewResult> produce(const PreviewRequest& request) const override {
        return guarded([&]() -> Result<PreviewResult> {
            PreviewResult out;
            if (request.purpose == PreviewPurpose::Thumbnail) {
                out.content = PlaceholderPreview{PlaceholderReason::NoThumbnailer,
                                                 "no video decoder or thumbnailer"};
            } else {
                out.content = VideoPreview{};
            }
            return out;
        });
    }
};

// Limits concurrent thumbnailer processes across providers; waits are cancellable.
class ProcessSlots {
public:
    explicit ProcessSlots(std::size_t max) : free_(std::max<std::size_t>(max, 1)) {}
    bool acquire(const std::stop_token& stop) {
        std::unique_lock lock(mutex_);
        if (!cv_.wait(lock, stop, [&] { return free_ > 0; })) {
            return false;
        }
        --free_;
        return true;
    }
    void release() {
        {
            const std::lock_guard lock(mutex_);
            ++free_;
        }
        cv_.notify_one();
    }

private:
    std::mutex mutex_;
    std::condition_variable_any cv_;
    std::size_t free_;
};

class ThumbnailerProvider final : public IPreviewProvider {
public:
    ThumbnailerProvider(thumbnail::Thumbnailer t, std::shared_ptr<platform::IProcessRunner> runner,
                        thumbnail::ThumbnailerLimits limits, std::shared_ptr<ProcessSlots> slots)
        : thumbnailer_(std::move(t)), id_("thumbnailer." + thumbnailer_.id),
          runner_(std::move(runner)), limits_(limits), slots_(std::move(slots)) {}

    [[nodiscard]] std::string_view id() const noexcept override { return id_; }
    [[nodiscard]] int can_handle(std::string_view mime, const fs::DirEntry& entry,
                                 PreviewPurpose /*purpose*/) const noexcept override {
        if (!is_regular_like(entry)) {
            return kCannotHandle;
        }
        return thumbnail::choose_thumbnailer(Span<const thumbnail::Thumbnailer>(&thumbnailer_, 1),
                                             mime) != nullptr
                   ? kPriorityExternal
                   : kCannotHandle;
    }
    [[nodiscard]] Result<PreviewResult> produce(const PreviewRequest& request) const override {
        return guarded([&]() -> Result<PreviewResult> {
            thumbnail::SourceInfo source;
            source.path = request.path;
            auto uri = thumbnail::file_uri(request.path);
            if (!uri) {
                return Unexpected<Error>(uri.error());
            }
            source.uri = std::move(*uri);
            source.mtime = request.entry.mtime;
            source.size = request.entry.size;
            source.mime = request.mime;
            const std::uint32_t side = std::max(request.target.width, request.target.height);
            const thumbnail::ThumbnailSize cls =
                side == 0 ? thumbnail::ThumbnailSize::XXLarge : thumbnail::size_for(side);
            if (!slots_->acquire(request.stop)) {
                return fail(ErrorCode::Cancelled, "cancelled");
            }
            auto img = thumbnail::run_thumbnailer(thumbnailer_, source, cls, *runner_, limits_,
                                                  request.stop);
            slots_->release();
            if (!img) {
                Error e = img.error();
                if (e.code == ErrorCode::IoError) {
                    e.code = ErrorCode::InvalidArgument; // the thumbnailer failed on this file
                }
                return Unexpected<Error>(std::move(e));
            }
            ImageBuffer image = std::move(*img);
            if (!request.target.empty()) {
                const PixelSize fit = fit_size(image.size(), request.target);
                if (fit != image.size()) {
                    auto d = downscale(image, fit);
                    if (!d) {
                        return Unexpected<Error>(d.error());
                    }
                    image = std::move(*d);
                }
            }
            PreviewResult out;
            if (starts_with(request.mime, "video/")) {
                VideoPreview v;
                v.poster = std::move(image);
                v.poster_origin = ImageOrigin::ExternalThumbnailer;
                out.content = std::move(v);
            } else {
                ImagePreview ip;
                ip.info.stored = image.size();
                ip.info.display = image.size();
                ip.image = std::move(image);
                ip.origin = ImageOrigin::ExternalThumbnailer;
                out.content = std::move(ip);
            }
            return out;
        });
    }

private:
    thumbnail::Thumbnailer thumbnailer_;
    std::string id_;
    std::shared_ptr<platform::IProcessRunner> runner_;
    thumbnail::ThumbnailerLimits limits_;
    std::shared_ptr<ProcessSlots> slots_;
};

} // namespace

BuildFeatures build_features() noexcept {
    BuildFeatures f;
    f.jpeg = true;
    f.png = true;
    f.webp = true;
    f.gif = true;
    f.bmp = true;
    f.exif = true;
    static const bool heif = detail::heif_has_hevc();
    static const bool avif = detail::heif_has_av1();
    f.heif = heif;
    f.avif = avif;
#if defined(NAVIGATOR_HAVE_FFMPEG)
    f.ffmpeg = true;
#endif
#if defined(NAVIGATOR_HAVE_RSVG)
    f.svg = true;
#endif
    return f;
}

Result<void> register_builtin_providers(ProviderRegistry& registry,
                                        const BuiltinProviderOptions& options) {
    try {
        const BuildFeatures f = build_features();
        const std::uint64_t budget = options.decode_memory_bytes;
        std::vector<std::unique_ptr<IPreviewProvider>> list;
        list.push_back(std::make_unique<ImageProvider>(
            "image.jpeg", std::vector<std::string>{"image/jpeg", "image/pjpeg"}, budget));
        list.push_back(std::make_unique<ImageProvider>(
            "image.png", std::vector<std::string>{"image/png", "image/apng"}, budget));
        list.push_back(std::make_unique<ImageProvider>(
            "image.webp", std::vector<std::string>{"image/webp"}, budget));
        list.push_back(std::make_unique<ImageProvider>(
            "image.gif", std::vector<std::string>{"image/gif"}, budget));
        list.push_back(std::make_unique<ImageProvider>(
            "image.bmp", std::vector<std::string>{"image/bmp", "image/x-bmp", "image/x-ms-bmp"},
            budget));
        if (f.heif || f.avif) {
            std::vector<std::string> mimes;
            if (f.heif) {
                mimes.insert(mimes.end(), {"image/heif", "image/heic", "image/heif-sequence",
                                           "image/heic-sequence"});
            }
            if (f.avif) {
                mimes.insert(mimes.end(), {"image/avif", "image/avif-sequence"});
            }
            list.push_back(std::make_unique<ImageProvider>("image.heif", std::move(mimes), budget));
        }
        if (f.svg) {
            list.push_back(std::make_unique<ImageProvider>(
                "image.svg", std::vector<std::string>{"image/svg+xml", "image/svg+xml-compressed"},
                budget, &detail::decode_svg_file));
        }
        if (f.ffmpeg) {
            list.push_back(std::make_unique<VideoProvider>(budget));
        }
        list.push_back(std::make_unique<TextProvider>(budget));
        list.push_back(std::make_unique<FolderProvider>());
        if (options.runner) {
            auto slots = std::make_shared<ProcessSlots>(options.thumbnailer_limits.max_concurrent);
            for (const thumbnail::Thumbnailer& t : options.thumbnailers) {
                list.push_back(std::make_unique<ThumbnailerProvider>(
                    t, options.runner, options.thumbnailer_limits, slots));
            }
        }
        list.push_back(std::make_unique<ComingSoonProvider>(!f.svg));
        list.push_back(std::make_unique<VideoPlaceholderProvider>());
        for (auto& p : list) {
            auto r = registry.add(std::move(p));
            if (!r) {
                return r;
            }
        }
        return ok();
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "register_builtin_providers: allocation failed");
    }
}

} // namespace navigator::preview
