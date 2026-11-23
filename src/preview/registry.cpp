// src/preview/registry.cpp: PreviewKey (make_key, hash_value), approx_bytes and the
// ProviderRegistry.
#include <algorithm>
#include <exception>
#include <functional>
#include <new>

#include <navigator/preview.hpp>

namespace navigator::preview {

namespace {

constexpr std::uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

void mix(std::uint64_t& h, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (static_cast<unsigned>(i) * 8U)) & 0xFFU;
        h *= kFnvPrime;
    }
}

[[nodiscard]] std::uint64_t folder_hash(const FolderOptions& f) noexcept {
    std::uint64_t h = kFnvOffset;
    mix(h, 0x464F4C44U); // "FOLD"
    mix(h, f.show_hidden ? 1U : 0U);
    mix(h, static_cast<std::uint64_t>(f.sort.key));
    mix(h, (f.sort.descending ? 1U : 0U) | (f.sort.folders_first ? 2U : 0U) |
               (f.sort.natural ? 4U : 0U) | (f.sort.case_insensitive ? 8U : 0U));
    mix(h, f.max_entries);
    return h;
}

[[nodiscard]] std::uint64_t text_hash(const TextOptions& t) noexcept {
    std::uint64_t h = kFnvOffset;
    mix(h, 0x54455854U); // "TEXT"
    mix(h, t.max_bytes);
    mix(h, t.max_row_bytes);
    mix(h, t.max_rows);
    return h;
}

[[nodiscard]] std::size_t image_bytes(const ImageBuffer& b) noexcept { return b.pixels.capacity(); }

[[nodiscard]] std::size_t exif_bytes(const std::optional<ExifInfo>& e) noexcept {
    if (!e) {
        return 0;
    }
    return sizeof(ExifInfo) + e->make.capacity() + e->model.capacity() + e->lens_model.capacity() +
           e->software.capacity();
}

} // namespace

PreviewKey make_key(const PreviewRequest& request) {
    PreviewKey key;
    key.path = request.path;
    key.mtime = request.entry.mtime;
    key.size = request.entry.size;
    key.target = request.target;
    key.purpose = request.purpose;
    if (request.purpose == PreviewPurpose::Thumbnail) {
        key.options = 0;
    } else if (fs::is_directory_like(request.entry)) {
        key.options = folder_hash(request.folder);
    } else {
        key.options = text_hash(request.text);
    }
    return key;
}

std::size_t hash_value(const PreviewKey& key) noexcept {
    std::uint64_t h = kFnvOffset;
    mix(h, std::hash<std::string>{}(key.path.native()));
    mix(h, static_cast<std::uint64_t>(key.mtime.time_since_epoch().count()));
    mix(h, key.size);
    mix(h, (std::uint64_t{key.target.width} << 32U) | key.target.height);
    mix(h, static_cast<std::uint64_t>(key.purpose));
    mix(h, key.options);
    return static_cast<std::size_t>(h);
}

std::size_t approx_bytes(const PreviewResult& result) noexcept {
    std::size_t total = sizeof(PreviewResult) + result.provider_id.capacity();
    struct Visitor {
        std::size_t operator()(const PlaceholderPreview& p) const noexcept {
            return p.detail.capacity();
        }
        std::size_t operator()(const ImagePreview& p) const noexcept {
            return image_bytes(p.image) + exif_bytes(p.info.exif);
        }
        std::size_t operator()(const VideoPreview& p) const noexcept {
            return image_bytes(p.poster) + p.info.container.capacity() +
                   p.info.container_long.capacity() + p.info.video_codec.capacity() +
                   p.info.audio_codec.capacity();
        }
        std::size_t operator()(const TextPreview& p) const noexcept {
            return p.text.capacity() + p.rows.capacity() * sizeof(TextRow);
        }
        std::size_t operator()(const FolderPreview& p) const noexcept {
            std::size_t n = p.entries.capacity() * sizeof(fs::DirEntry);
            for (const fs::DirEntry& e : p.entries) {
                if (e.name.capacity() > 15) { // beyond the small-string buffer
                    n += e.name.capacity();
                }
            }
            return n;
        }
    };
    total += std::visit(Visitor{}, result.content);
    return total;
}

// =============================================================================================
// ProviderRegistry
// =============================================================================================

Result<void> ProviderRegistry::add(std::unique_ptr<IPreviewProvider> provider) {
    if (!provider || provider->id().empty()) {
        return fail(ErrorCode::InvalidArgument, "ProviderRegistry::add: null provider or empty id");
    }
    if (find(provider->id()) != nullptr) {
        return fail(ErrorCode::AlreadyExists,
                    "ProviderRegistry::add: duplicate id " + std::string(provider->id()));
    }
    try {
        providers_.push_back(std::move(provider));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "ProviderRegistry::add: allocation failed");
    }
    return ok();
}

const IPreviewProvider* ProviderRegistry::find(std::string_view id) const noexcept {
    for (const auto& p : providers_) {
        if (p->id() == id) {
            return p.get();
        }
    }
    return nullptr;
}

std::vector<const IPreviewProvider*> ProviderRegistry::candidates(std::string_view mime,
                                                                  const fs::DirEntry& entry,
                                                                  PreviewPurpose purpose) const {
    if (mime.empty()) {
        mime = "application/octet-stream";
    }
    std::vector<std::pair<int, const IPreviewProvider*>> scored;
    for (const auto& p : providers_) {
        const int prio = p->can_handle(mime, entry, purpose);
        if (prio > kCannotHandle) {
            scored.emplace_back(prio, p.get());
        }
    }
    std::stable_sort(scored.begin(), scored.end(),
                     [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<const IPreviewProvider*> out;
    out.reserve(scored.size());
    for (const auto& s : scored) {
        out.push_back(s.second);
    }
    return out;
}

Result<PreviewResult> ProviderRegistry::produce(const PreviewRequest& request) const {
    try {
        if (request.stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "cancelled");
        }
        const auto list = candidates(request.mime, request.entry, request.purpose);
        for (const IPreviewProvider* p : list) {
            if (request.stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "cancelled");
            }
            auto r = p->produce(request);
            if (r) {
                r->provider_id = std::string(p->id());
                return r;
            }
            if (r.error().code != ErrorCode::Unsupported) {
                return r;
            }
        }
        if (request.stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "cancelled");
        }
        PreviewResult placeholder;
        placeholder.content = PlaceholderPreview{PlaceholderReason::NoProvider, "no provider"};
        return placeholder;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "ProviderRegistry::produce: allocation failed");
    } catch (const std::exception& e) {
        return fail(ErrorCode::Unknown, std::string("ProviderRegistry::produce: ") + e.what());
    }
}

} // namespace navigator::preview
