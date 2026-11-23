// src/preview/decode_video.cpp: video poster frame + VideoInfo through FFmpeg (optional build,
// NAVIGATOR_HAVE_FFMPEG). Only the "file" protocol is allowed; probing is bounded
// (probesize / analyzeduration); every blocking FFmpeg call is interrupted through the
// interrupt callback by the stop token or a 10 s wall-clock deadline. The poster is the first
// frame decoded after seeking (backwards, to a key frame) to 10% of the duration, at most 30 s;
// if that fails the file is decoded from the start. Packets read for the poster are capped.
#include "internal.hpp"

#if defined(NAVIGATOR_HAVE_FFMPEG)
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <new>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
#endif

namespace navigator::preview::detail {

#if defined(NAVIGATOR_HAVE_FFMPEG)

namespace {

constexpr auto kDeadline = std::chrono::seconds(10);
constexpr int kMaxPackets = 3000;
constexpr std::int64_t kSeekCapUs = std::int64_t{30} * AV_TIME_BASE;

struct Interrupt {
    std::stop_token stop;
    std::chrono::steady_clock::time_point deadline;
};

int interrupt_cb(void* opaque) {
    const auto* irq = static_cast<const Interrupt*>(opaque);
    return irq->stop.stop_requested() || std::chrono::steady_clock::now() > irq->deadline ? 1 : 0;
}

struct FormatDeleter {
    void operator()(AVFormatContext* f) const noexcept { avformat_close_input(&f); }
};
struct CodecDeleter {
    void operator()(AVCodecContext* c) const noexcept { avcodec_free_context(&c); }
};
struct FrameDeleter {
    void operator()(AVFrame* f) const noexcept { av_frame_free(&f); }
};
struct PacketDeleter {
    void operator()(AVPacket* p) const noexcept { av_packet_free(&p); }
};
struct SwsDeleter {
    void operator()(SwsContext* s) const noexcept { sws_freeContext(s); }
};

void quiet_ffmpeg() {
    static std::once_flag once;
    std::call_once(once, [] { av_log_set_level(AV_LOG_QUIET); });
}

[[nodiscard]] std::int16_t stream_rotation(const AVStream* st) noexcept {
    const std::uint8_t* matrix = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 29, 100)
    const AVPacketSideData* sd = av_packet_side_data_get(
        st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
    if (sd != nullptr && sd->size >= 9 * sizeof(std::int32_t)) {
        matrix = sd->data;
    }
#else
    std::size_t size = 0;
    matrix = av_stream_get_side_data(st, AV_PKT_DATA_DISPLAYMATRIX, &size);
    if (size < 9 * sizeof(std::int32_t)) {
        matrix = nullptr;
    }
#endif
    if (matrix == nullptr) {
        return 0;
    }
    const double ccw = av_display_rotation_get(reinterpret_cast<const std::int32_t*>(matrix));
    if (!std::isfinite(ccw)) {
        return 0;
    }
    // av_display_rotation_get is counter-clockwise; we report clockwise in 90-degree steps.
    long cw = std::lround(-ccw / 90.0) * 90L;
    cw %= 360L;
    if (cw < 0) {
        cw += 360L;
    }
    return static_cast<std::int16_t>(cw);
}

[[nodiscard]] ExifOrientation rotation_orientation(std::int16_t cw) noexcept {
    switch (cw) {
    case 90: return ExifOrientation::Rotate90;
    case 180: return ExifOrientation::Rotate180;
    case 270: return ExifOrientation::Rotate270;
    default: return ExifOrientation::Normal;
    }
}

[[nodiscard]] Error av_failure(ErrorCode code, std::string_view what, int err, const Path& path) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return Error{code, "video: " + std::string(what) + ": " + buf + ": " + path.native()};
}

// Decodes the first frame from the current position. 1: frame, 0: none, <0: AVERROR.
int decode_one(AVFormatContext* fmt, AVCodecContext* codec, int vidx, AVPacket* pkt,
               AVFrame* frame) {
    int packets = 0;
    bool flushed = false;
    for (;;) {
        int rc = avcodec_receive_frame(codec, frame);
        if (rc == 0) {
            return 1;
        }
        if (rc == AVERROR_EOF) {
            return 0;
        }
        if (rc != AVERROR(EAGAIN)) {
            return rc;
        }
        if (flushed) {
            return 0;
        }
        if (++packets > kMaxPackets) {
            return 0;
        }
        rc = av_read_frame(fmt, pkt);
        if (rc < 0) {
            if (rc == AVERROR_EXIT) {
                return rc;
            }
            (void)avcodec_send_packet(codec, nullptr); // flush at EOF / read error
            flushed = true;
            continue;
        }
        if (pkt->stream_index == vidx) {
            rc = avcodec_send_packet(codec, pkt);
            av_packet_unref(pkt);
            if (rc < 0 && rc != AVERROR(EAGAIN) && rc != AVERROR_INVALIDDATA) {
                return rc;
            }
        } else {
            av_packet_unref(pkt);
        }
    }
}

} // namespace

Result<VideoPreview> decode_video_file(const Path& path, const DecodeParams& p) {
    quiet_ffmpeg();
    {
        auto fd = open_regular(path, nullptr);
        if (!fd) {
            return Unexpected<Error>(fd.error());
        }
    }
    try {
        Interrupt irq{p.stop, std::chrono::steady_clock::now() + kDeadline};
        AVFormatContext* raw = avformat_alloc_context();
        if (raw == nullptr) {
            return fail(ErrorCode::OutOfMemory, "video: cannot allocate a format context");
        }
        raw->interrupt_callback.callback = interrupt_cb;
        raw->interrupt_callback.opaque = &irq;
        raw->probesize = std::int64_t{10} << 20U;
        raw->max_analyze_duration = std::int64_t{5} * AV_TIME_BASE;
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "protocol_whitelist", "file", 0);
        const std::string url = "file:" + path.native();
        int rc = avformat_open_input(&raw, url.c_str(), nullptr, &opts);
        av_dict_free(&opts);
        if (rc < 0) { // raw is freed by avformat_open_input on failure
            if (p.stop.stop_requested()) {
                return fail(ErrorCode::Cancelled, "cancelled");
            }
            return Unexpected<Error>(
                av_failure(rc == AVERROR(ENOMEM) ? ErrorCode::OutOfMemory : ErrorCode::Unsupported,
                           "open", rc, path));
        }
        std::unique_ptr<AVFormatContext, FormatDeleter> fmt(raw);
        rc = avformat_find_stream_info(fmt.get(), nullptr);
        if (p.stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "cancelled");
        }
        if (rc < 0) {
            return Unexpected<Error>(av_failure(ErrorCode::InvalidArgument, "probe", rc, path));
        }
        VideoPreview out;
        VideoInfo& info = out.info;
        info.source = MetadataSource::FFmpeg;
        if (fmt->iformat != nullptr) {
            info.container = fmt->iformat->name != nullptr ? fmt->iformat->name : "";
            info.container_long = fmt->iformat->long_name != nullptr ? fmt->iformat->long_name : "";
        }
        if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0) {
            info.duration_ms = fmt->duration / (AV_TIME_BASE / 1000);
        }
        info.bit_rate = fmt->bit_rate > 0 ? static_cast<std::uint64_t>(fmt->bit_rate) : 0U;
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            const AVStream* st = fmt->streams[i];
            const AVCodecParameters* par = st->codecpar;
            if (par->codec_type == AVMEDIA_TYPE_VIDEO &&
                (st->disposition & AV_DISPOSITION_ATTACHED_PIC) == 0) {
                ++info.video_streams;
            } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
                if (info.audio_streams == 0) {
                    info.audio_codec = avcodec_get_name(par->codec_id);
                    info.audio_channels =
                        static_cast<std::uint32_t>(std::max(par->ch_layout.nb_channels, 0));
                    info.audio_sample_rate =
                        static_cast<std::uint32_t>(std::max(par->sample_rate, 0));
                }
                ++info.audio_streams;
            } else if (par->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                ++info.subtitle_streams;
            }
        }
        const AVCodec* decoder = nullptr;
        const int vidx = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
        if (vidx < 0) {
            if (vidx != AVERROR_DECODER_NOT_FOUND || p.purpose == PreviewPurpose::Thumbnail) {
                return fail(ErrorCode::Unsupported,
                            "video: no decodable video stream: " + path.native());
            }
            return out; // metadata only
        }
        AVStream* st = fmt->streams[vidx];
        const AVCodecParameters* par = st->codecpar;
        info.video_codec = avcodec_get_name(par->codec_id);
        AVRational fr = st->avg_frame_rate;
        if (fr.num <= 0 || fr.den <= 0) {
            fr = st->r_frame_rate;
        }
        if (fr.num > 0 && fr.den > 0) {
            info.frame_rate =
                Rational{static_cast<std::uint32_t>(fr.num), static_cast<std::uint32_t>(fr.den)};
        }
        info.rotation_degrees = stream_rotation(st);
        const PixelSize coded{static_cast<std::uint32_t>(std::max(par->width, 0)),
                              static_cast<std::uint32_t>(std::max(par->height, 0))};
        PixelSize unrotated = coded;
        const AVRational sar = av_guess_sample_aspect_ratio(fmt.get(), st, nullptr);
        if (sar.num > 0 && sar.den > 0 && sar.num != sar.den && !coded.empty()) {
            const double w = static_cast<double>(coded.width) * sar.num / sar.den;
            unrotated.width = static_cast<std::uint32_t>(std::clamp(std::lround(w), 1L, 65535L));
        }
        const ExifOrientation orient = rotation_orientation(info.rotation_degrees);
        info.frame = oriented_size(unrotated, orient);
        if (decoder == nullptr || coded.empty()) {
            if (p.purpose == PreviewPurpose::Thumbnail) {
                return fail(ErrorCode::Unsupported, "video: no decoder: " + path.native());
            }
            return out;
        }
        if (auto ok = check_source_size(coded, "video"); !ok) {
            return Unexpected<Error>(ok.error());
        }
        if (coded.pixels() * 4U * 4U > p.memory_budget) {
            return fail(ErrorCode::OutOfMemory, "video: frame too large: " + path.native());
        }

        std::unique_ptr<AVCodecContext, CodecDeleter> codec(avcodec_alloc_context3(decoder));
        if (!codec) {
            return fail(ErrorCode::OutOfMemory, "video: cannot allocate a codec context");
        }
        if (avcodec_parameters_to_context(codec.get(), par) < 0) {
            return fail(ErrorCode::InvalidArgument,
                        "video: bad codec parameters: " + path.native());
        }
        codec->thread_count = 1;
        codec->max_pixels = static_cast<std::int64_t>(kMaxSourcePixels);
        rc = avcodec_open2(codec.get(), decoder, nullptr);
        if (rc < 0) {
            if (p.purpose == PreviewPurpose::Thumbnail) {
                return Unexpected<Error>(av_failure(ErrorCode::Unsupported, "codec", rc, path));
            }
            return out;
        }
        std::unique_ptr<AVPacket, PacketDeleter> pkt(av_packet_alloc());
        std::unique_ptr<AVFrame, FrameDeleter> frame(av_frame_alloc());
        if (!pkt || !frame) {
            return fail(ErrorCode::OutOfMemory, "video: allocation failed");
        }
        int got = 0;
        if (info.duration_ms && *info.duration_ms > 0) {
            const std::int64_t ts = std::min(fmt->duration / 10, kSeekCapUs);
            if (ts > 0 && av_seek_frame(fmt.get(), -1, ts, AVSEEK_FLAG_BACKWARD) >= 0) {
                avcodec_flush_buffers(codec.get());
                got = decode_one(fmt.get(), codec.get(), vidx, pkt.get(), frame.get());
            }
        }
        if (got <= 0 && !p.stop.stop_requested()) {
            (void)av_seek_frame(fmt.get(), vidx, 0, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(codec.get());
            got = decode_one(fmt.get(), codec.get(), vidx, pkt.get(), frame.get());
        }
        if (p.stop.stop_requested()) {
            return fail(ErrorCode::Cancelled, "cancelled");
        }
        if (got <= 0) {
            if (p.purpose == PreviewPurpose::Thumbnail) {
                return fail(ErrorCode::InvalidArgument,
                            "video: no decodable frame: " + path.native());
            }
            return out;
        }
        const PixelSize fsize{static_cast<std::uint32_t>(std::max(frame->width, 0)),
                              static_cast<std::uint32_t>(std::max(frame->height, 0))};
        if (fsize.empty() || fsize.width > kMaxSourceDimension ||
            fsize.height > kMaxSourceDimension || frame->format < 0) {
            return fail(ErrorCode::InvalidArgument, "video: bad frame: " + path.native());
        }
        // Scale size in unrotated orientation, aspect from the display size.
        PixelSize target_unrotated = oriented_size(p.target, orient);
        PixelSize out_size = fit_size(unrotated, target_unrotated);
        std::unique_ptr<SwsContext, SwsDeleter> sws(
            sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                           static_cast<int>(out_size.width), static_cast<int>(out_size.height),
                           AV_PIX_FMT_RGBA, SWS_AREA, nullptr, nullptr, nullptr));
        if (!sws) {
            return fail(ErrorCode::Unsupported,
                        "video: unsupported pixel format: " + path.native());
        }
        auto image = make_image(out_size);
        if (!image) {
            return Unexpected<Error>(image.error());
        }
        std::uint8_t* dst[4] = {image->pixels.data(), nullptr, nullptr, nullptr};
        int dst_stride[4] = {static_cast<int>(image->stride), 0, 0, 0};
        if (sws_scale(sws.get(), frame->data, frame->linesize, 0, frame->height, dst, dst_stride) <=
            0) {
            return fail(ErrorCode::InvalidArgument, "video: scaling failed: " + path.native());
        }
        const AVPixFmtDescriptor* desc =
            av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
        const bool alpha = desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_ALPHA) != 0U;
        if (alpha) {
            premultiply_row(image->pixels.data(), static_cast<std::size_t>(out_size.pixels()));
        }
        image->opaque = !alpha;
        auto rotated = apply_exif_orientation(std::move(*image), orient);
        if (!rotated) {
            return Unexpected<Error>(rotated.error());
        }
        out.poster = std::move(*rotated);
        out.poster_origin = ImageOrigin::Decoded;
        return out;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "video: allocation failed");
    }
}

#else // !NAVIGATOR_HAVE_FFMPEG

Result<VideoPreview> decode_video_file(const Path& path, const DecodeParams& /*p*/) {
    return fail(ErrorCode::Unsupported, "video: built without FFmpeg: " + path.native());
}

#endif

} // namespace navigator::preview::detail
