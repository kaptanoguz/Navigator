// src/preview/decode_webp.cpp: WebP through libwebp. Static images decode incrementally (stop
// checked every 1 MiB of input) with libwebp's own scaler straight into the output size, into
// our zeroed buffer (a truncated file leaves the undecoded rows transparent). Animated images:
// first frame composed by WebPAnimDecoder, canvas bounded by the full-decode limit. EXIF / ICCP
// come from the RIFF chunks (WebPDemux).
#include <cstring>
#include <memory>
#include <new>

#include <webp/decode.h>
#include <webp/demux.h>

#include "internal.hpp"

namespace navigator::preview::detail {

namespace {

constexpr std::size_t kFeedChunk = std::size_t{1} << 20U;

struct DemuxDeleter {
    void operator()(WebPDemuxer* d) const noexcept { WebPDemuxDelete(d); }
};
struct IDecDeleter {
    void operator()(WebPIDecoder* d) const noexcept { WebPIDelete(d); }
};
struct AnimDeleter {
    void operator()(WebPAnimDecoder* d) const noexcept { WebPAnimDecoderDelete(d); }
};

} // namespace

Result<ImagePreview> decode_webp_file(const Path& path, const DecodeParams& p) {
    std::uint64_t file_size = 0;
    auto fd = open_regular(path, &file_size);
    if (!fd) {
        return Unexpected<Error>(fd.error());
    }
    // RIFF sizes are 32-bit; the whole file is needed in memory by libwebp.
    const std::uint64_t max_file = std::min<std::uint64_t>(p.memory_budget / 2U, 0xFFFFFFFFULL);
    if (file_size > max_file) {
        return fail(ErrorCode::OutOfMemory, "webp: file too large: " + path.native());
    }
    auto bytes = read_prefix(fd->get(), max_file, p.stop, path);
    if (!bytes) {
        return Unexpected<Error>(bytes.error());
    }
    try {
        const std::vector<std::uint8_t>& data = *bytes;
        WebPBitstreamFeatures features{};
        if (WebPGetFeatures(data.data(), data.size(), &features) != VP8_STATUS_OK) {
            return fail(ErrorCode::InvalidArgument, "webp: invalid header: " + path.native());
        }
        const PixelSize stored{static_cast<std::uint32_t>(std::max(features.width, 0)),
                               static_cast<std::uint32_t>(std::max(features.height, 0))};
        if (auto ok = check_source_size(stored, "webp"); !ok) {
            return Unexpected<Error>(ok.error());
        }
        ImagePreview out;
        out.info.format = ImageFormat::WebP;
        out.info.stored = stored;
        out.info.has_alpha = features.has_alpha != 0;
        out.info.animated = features.has_animation != 0;

        WebPData wd{data.data(), data.size()};
        std::unique_ptr<WebPDemuxer, DemuxDeleter> demux(WebPDemuxPartial(&wd, nullptr));
        if (demux) {
            const std::uint32_t flags = WebPDemuxGetI(demux.get(), WEBP_FF_FORMAT_FLAGS);
            out.info.has_icc_profile = (flags & ICCP_FLAG) != 0U;
            if (out.info.animated) {
                out.info.frame_count = WebPDemuxGetI(demux.get(), WEBP_FF_FRAME_COUNT);
            }
            WebPChunkIterator it{};
            if ((flags & EXIF_FLAG) != 0U && WebPDemuxGetChunk(demux.get(), "EXIF", 1, &it) != 0) {
                out.info.exif = parse_exif(it.chunk.bytes, it.chunk.size);
                WebPDemuxReleaseChunkIterator(&it);
            }
        }
        if (out.info.exif) {
            out.info.orientation = out.info.exif->orientation;
        }
        out.info.display = oriented_size(stored, out.info.orientation);
        const PixelSize out_display = output_display_size(out.info.display, p.target);
        const PixelSize out_stored = oriented_size(out_display, out.info.orientation);

        if (out.info.animated) {
            demux.reset();
            if (stored.pixels() > full_decode_limit(p)) {
                return fail(ErrorCode::OutOfMemory,
                            "webp: animation canvas too large: " + path.native());
            }
            WebPAnimDecoderOptions aopt{};
            if (WebPAnimDecoderOptionsInit(&aopt) == 0) {
                return fail(ErrorCode::Unknown, "webp: libwebp version mismatch");
            }
            aopt.color_mode = MODE_rgbA;
            aopt.use_threads = 0;
            std::unique_ptr<WebPAnimDecoder, AnimDeleter> anim(WebPAnimDecoderNew(&wd, &aopt));
            if (!anim) {
                return fail(ErrorCode::InvalidArgument,
                            "webp: invalid animation: " + path.native());
            }
            WebPAnimInfo ainfo{};
            if (WebPAnimDecoderGetInfo(anim.get(), &ainfo) == 0 ||
                PixelSize{ainfo.canvas_width, ainfo.canvas_height} != stored) {
                return fail(ErrorCode::InvalidArgument,
                            "webp: invalid animation: " + path.native());
            }
            out.info.frame_count = ainfo.frame_count;
            std::uint8_t* frame = nullptr;
            int timestamp = 0;
            if (p.stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "cancelled");
            }
            if (WebPAnimDecoderGetNext(anim.get(), &frame, &timestamp) == 0 || frame == nullptr) {
                return fail(ErrorCode::InvalidArgument,
                            "webp: cannot decode the first frame: " + path.native());
            }
            auto img = scale_and_orient(frame, stored, std::size_t{stored.width} * 4U, true,
                                        out.info.orientation, p.target);
            if (!img) {
                return Unexpected<Error>(img.error());
            }
            out.image = std::move(*img);
            return out;
        }

        auto buffer = make_image(out_stored);
        if (!buffer) {
            return Unexpected<Error>(buffer.error());
        }
        WebPDecoderConfig config{};
        if (WebPInitDecoderConfig(&config) == 0) {
            return fail(ErrorCode::Unknown, "webp: libwebp version mismatch");
        }
        config.options.use_threads = 0;
        if (out_stored != stored) {
            config.options.use_scaling = 1;
            config.options.scaled_width = static_cast<int>(out_stored.width);
            config.options.scaled_height = static_cast<int>(out_stored.height);
        }
        config.output.colorspace = MODE_rgbA;
        config.output.is_external_memory = 1;
        config.output.u.RGBA.rgba = buffer->pixels.data();
        config.output.u.RGBA.stride = static_cast<int>(buffer->stride);
        config.output.u.RGBA.size = buffer->pixels.size();
        std::unique_ptr<WebPIDecoder, IDecDeleter> idec(WebPIDecode(nullptr, 0, &config));
        if (!idec) {
            return fail(ErrorCode::OutOfMemory, "webp: cannot create the decoder");
        }
        VP8StatusCode status = VP8_STATUS_SUSPENDED;
        std::size_t fed = 0;
        while (fed < data.size()) {
            if (p.stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "cancelled");
            }
            fed = std::min(data.size(), fed + kFeedChunk);
            status = WebPIUpdate(idec.get(), data.data(), fed);
            if (status != VP8_STATUS_SUSPENDED) {
                break;
            }
        }
        int last_y = 0;
        if (status != VP8_STATUS_OK) {
            int w = 0;
            int h = 0;
            int stride = 0;
            if (WebPIDecGetRGB(idec.get(), &last_y, &w, &h, &stride) == nullptr) {
                last_y = 0;
            }
            if (status == VP8_STATUS_OUT_OF_MEMORY) {
                return fail(ErrorCode::OutOfMemory, "webp: out of memory");
            }
            if (last_y <= 0) {
                return fail(ErrorCode::InvalidArgument,
                            "webp: corrupt or truncated file: " + path.native());
            }
        }
        idec.reset();
        ImageBuffer img = std::move(*buffer);
        img.opaque = !features.has_alpha && status == VP8_STATUS_OK;
        auto oriented = apply_exif_orientation(std::move(img), out.info.orientation);
        if (!oriented) {
            return Unexpected<Error>(oriented.error());
        }
        out.image = std::move(*oriented);
        return out;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "webp: allocation failed");
    }
}

} // namespace navigator::preview::detail
