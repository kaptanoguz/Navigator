// tests/preview/video_clip.hpp: writes a tiny MPEG-4-in-Matroska clip with libavcodec /
// libavformat for the video tests and timings (FFmpeg builds only).
#pragma once

#if defined(NAVIGATOR_HAVE_FFMPEG)
#include <cstdint>
#include <cstdlib>
#include <filesystem>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
}

namespace navigator::test {

// Writes `frames` frames of w x h at 10 fps: left half bright, right half dark (Y plane), so the
// poster can be checked. Returns false when the encoder or muxer is unavailable.
inline bool write_clip(const std::filesystem::path& file, int w, int h, int frames,
                       const char* rotate = nullptr) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (codec == nullptr) {
        return false;
    }
    AVFormatContext* fmt = nullptr;
    if (avformat_alloc_output_context2(&fmt, nullptr, "matroska", file.c_str()) < 0 ||
        fmt == nullptr) {
        return false;
    }
    AVStream* st = avformat_new_stream(fmt, nullptr);
    AVCodecContext* enc = avcodec_alloc_context3(codec);
    enc->width = w;
    enc->height = h;
    enc->pix_fmt = AV_PIX_FMT_YUV420P;
    enc->time_base = AVRational{1, 10};
    enc->framerate = AVRational{10, 1};
    enc->gop_size = 5;
    enc->bit_rate = 400000;
    if ((fmt->oformat->flags & AVFMT_GLOBALHEADER) != 0) {
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    bool ok = avcodec_open2(enc, codec, nullptr) >= 0;
    ok = ok && avcodec_parameters_from_context(st->codecpar, enc) >= 0;
    st->time_base = enc->time_base;
    if (rotate != nullptr) {
        // Display matrix side data for a rotated clip.
        auto* sd = av_packet_side_data_new(&st->codecpar->coded_side_data,
                                           &st->codecpar->nb_coded_side_data,
                                           AV_PKT_DATA_DISPLAYMATRIX, sizeof(std::int32_t) * 9, 0);
        if (sd != nullptr) {
            av_display_rotation_set(reinterpret_cast<std::int32_t*>(sd->data), std::atof(rotate));
        }
    }
    ok = ok && avio_open(&fmt->pb, file.c_str(), AVIO_FLAG_WRITE) >= 0;
    ok = ok && avformat_write_header(fmt, nullptr) >= 0;
    AVFrame* frame = av_frame_alloc();
    frame->format = enc->pix_fmt;
    frame->width = w;
    frame->height = h;
    ok = ok && av_frame_get_buffer(frame, 0) >= 0;
    AVPacket* pkt = av_packet_alloc();
    const auto drain = [&] {
        while (avcodec_receive_packet(enc, pkt) == 0) {
            av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
            pkt->stream_index = st->index;
            av_interleaved_write_frame(fmt, pkt);
        }
    };
    for (int i = 0; ok && i < frames; ++i) {
        av_frame_make_writable(frame);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                frame->data[0][y * frame->linesize[0] + x] = x < w / 2 ? 235 : 16;
            }
        }
        for (int y = 0; y < h / 2; ++y) {
            for (int x = 0; x < w / 2; ++x) {
                frame->data[1][y * frame->linesize[1] + x] = 128;
                frame->data[2][y * frame->linesize[2] + x] = 128;
            }
        }
        frame->pts = i;
        ok = avcodec_send_frame(enc, frame) >= 0;
        drain();
    }
    avcodec_send_frame(enc, nullptr);
    drain();
    if (ok) {
        av_write_trailer(fmt);
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&enc);
    avio_closep(&fmt->pb);
    avformat_free_context(fmt);
    return ok;
}

} // namespace navigator::test
#endif
