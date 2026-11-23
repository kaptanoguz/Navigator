// src/preview/decode_gif.cpp: in-tree GIF decoder, first frame only (no new dependency). The
// first image is LZW-decoded into an index buffer, composed onto a transparent canvas of the
// logical screen size (bounded by the full-decode limit) and scaled. Further frames are only
// counted by skipping their data (at most kMaxCountBytes of input). Truncated data leaves the
// undecoded pixels transparent.
#include <array>
#include <cerrno>
#include <cstring>
#include <new>
#include <utility>

#include "internal.hpp"

namespace navigator::preview::detail {

namespace {

constexpr std::size_t kReadChunk = std::size_t{256} * 1024U;
constexpr std::uint64_t kMaxCountBytes = std::uint64_t{64} << 20U;
constexpr std::uint32_t kMaxCountFrames = 100000;

class Reader {
public:
    Reader(int fd, std::stop_token stop) : fd_(fd), stop_(std::move(stop)) {}

    // -1: EOF, -2: cancelled, -3: I/O error.
    int byte() {
        if (pos_ >= len_ && !refill()) {
            return status_;
        }
        return buf_[pos_++];
    }
    bool read(std::uint8_t* out, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            const int b = byte();
            if (b < 0) {
                return false;
            }
            out[i] = static_cast<std::uint8_t>(b);
        }
        return true;
    }
    bool skip(std::size_t n) {
        while (n > 0) {
            if (pos_ >= len_ && !refill()) {
                return false;
            }
            const std::size_t k = std::min(n, len_ - pos_);
            pos_ += k;
            n -= k;
        }
        return true;
    }
    [[nodiscard]] std::uint64_t consumed() const noexcept { return file_off_ - (len_ - pos_); }
    [[nodiscard]] int status() const noexcept { return status_; }
    [[nodiscard]] int saved_errno() const noexcept { return errno_; }

private:
    bool refill() {
        if (stop_.stop_requested()) {
            status_ = -2;
            return false;
        }
        const long long n = pread_full(fd_, buf_.data(), buf_.size(), file_off_);
        if (n < 0) {
            status_ = -3;
            errno_ = errno;
            return false;
        }
        if (n == 0) {
            status_ = -1;
            return false;
        }
        file_off_ += static_cast<std::uint64_t>(n);
        pos_ = 0;
        len_ = static_cast<std::size_t>(n);
        return true;
    }

    int fd_;
    std::stop_token stop_;
    std::array<std::uint8_t, kReadChunk> buf_{};
    std::size_t pos_ = 0;
    std::size_t len_ = 0;
    std::uint64_t file_off_ = 0;
    int status_ = -1;
    int errno_ = 0;
};

// Skips a sequence of data sub-blocks up to the terminator.
bool skip_sub_blocks(Reader& r) {
    for (;;) {
        const int n = r.byte();
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            return true;
        }
        if (!r.skip(static_cast<std::size_t>(n))) {
            return false;
        }
    }
}

// LZW decoder over sub-blocks writing indices into `out` (size w*h). Returns pixels written.
std::size_t lzw_decode(Reader& r, int min_code_size, std::vector<std::uint8_t>& out,
                       bool& data_ended) {
    data_ended = false;
    constexpr int kMaxCodes = 4096;
    std::array<std::uint16_t, kMaxCodes> prefix{};
    std::array<std::uint8_t, kMaxCodes> suffix{};
    std::array<std::uint8_t, kMaxCodes + 1> stack{};
    const int clear = 1 << min_code_size;
    const int end = clear + 1;
    int code_size = min_code_size + 1;
    int next = clear + 2;
    int old = -1;
    std::uint8_t first = 0;
    std::uint32_t bits = 0;
    int nbits = 0;
    int block_left = 0;
    std::size_t written = 0;
    const std::size_t total = out.size();
    for (int i = 0; i < clear; ++i) {
        suffix[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
    }
    bool finished = false;
    while (!finished) {
        while (nbits < code_size) {
            if (block_left == 0) {
                const int n = r.byte();
                if (n <= 0) {
                    data_ended = n == 0;
                    return written;
                }
                block_left = n;
            }
            const int b = r.byte();
            if (b < 0) {
                return written;
            }
            --block_left;
            bits |= static_cast<std::uint32_t>(b) << static_cast<unsigned>(nbits);
            nbits += 8;
        }
        const int code = static_cast<int>(bits & ((1U << static_cast<unsigned>(code_size)) - 1U));
        bits >>= static_cast<unsigned>(code_size);
        nbits -= code_size;
        if (code == clear) {
            code_size = min_code_size + 1;
            next = clear + 2;
            old = -1;
            continue;
        }
        if (code == end) {
            finished = true;
            break;
        }
        if (old < 0) {
            if (code >= clear) {
                return written; // invalid first code
            }
            if (written < total) {
                out[written++] = static_cast<std::uint8_t>(code);
            }
            old = code;
            first = static_cast<std::uint8_t>(code);
            continue;
        }
        int in = code;
        std::size_t sp = 0;
        if (code > next || (code == next && next >= kMaxCodes)) {
            return written; // corrupt
        }
        if (code == next) {
            stack[sp++] = first;
            in = old;
        }
        while (in >= clear) {
            if (sp >= stack.size() - 1) {
                return written;
            }
            stack[sp++] = suffix[static_cast<std::size_t>(in)];
            in = prefix[static_cast<std::size_t>(in)];
        }
        first = static_cast<std::uint8_t>(in);
        stack[sp++] = first;
        while (sp > 0) {
            const std::uint8_t v = stack[--sp];
            if (written < total) {
                out[written++] = v;
            }
        }
        if (next < kMaxCodes) {
            prefix[static_cast<std::size_t>(next)] = static_cast<std::uint16_t>(old);
            suffix[static_cast<std::size_t>(next)] = first;
            ++next;
            if (next == (1 << code_size) && code_size < 12) {
                ++code_size;
            }
        }
        old = code;
        if (written >= total) {
            break;
        }
    }
    // Consume the rest of the image data.
    if (block_left > 0) {
        (void)r.skip(static_cast<std::size_t>(block_left));
    }
    data_ended = skip_sub_blocks(r);
    return written;
}

[[nodiscard]] std::uint32_t le16(const std::uint8_t* p) noexcept {
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8U);
}

} // namespace

Result<ImagePreview> decode_gif_file(const Path& path, const DecodeParams& p) {
    auto fd = open_regular(path, nullptr);
    if (!fd) {
        return Unexpected<Error>(fd.error());
    }
    try {
        auto reader = std::make_unique<Reader>(fd->get(), p.stop);
        Reader& r = *reader;
        const auto read_failed = [&](std::string_view what) -> Error {
            if (r.status() == -2) {
                return Error{ErrorCode::Cancelled, "cancelled"};
            }
            if (r.status() == -3) {
                return errno_error(r.saved_errno(), "read", path);
            }
            return Error{ErrorCode::InvalidArgument,
                         "gif: " + std::string(what) + ": " + path.native()};
        };
        std::uint8_t header[13];
        if (!r.read(header, sizeof(header))) {
            return Unexpected<Error>(read_failed("truncated header"));
        }
        if (std::memcmp(header, "GIF87a", 6) != 0 && std::memcmp(header, "GIF89a", 6) != 0) {
            return fail(ErrorCode::InvalidArgument, "gif: bad signature: " + path.native());
        }
        PixelSize canvas{le16(header + 6), le16(header + 8)};
        const std::uint8_t flags = header[10];
        std::array<std::uint8_t, 768> global{};
        std::size_t global_count = 0;
        if ((flags & 0x80U) != 0U) {
            global_count = std::size_t{2} << (flags & 0x07U);
            if (!r.read(global.data(), global_count * 3U)) {
                return Unexpected<Error>(read_failed("truncated colour table"));
            }
        }
        int transparent = -1;
        ImagePreview out;
        out.info.format = ImageFormat::Gif;
        bool have_frame = false;
        std::uint32_t frames = 0;
        ImageBuffer canvas_img;
        for (;;) {
            const int block = r.byte();
            if (block < 0) {
                if (r.status() == -2) {
                    return fail(ErrorCode::Cancelled, "cancelled");
                }
                break;
            }
            if (block == 0x3B) { // trailer
                break;
            }
            if (block == 0x21) {
                const int label = r.byte();
                if (label < 0) {
                    break;
                }
                if (label == 0xF9 && !have_frame) {
                    std::uint8_t gce[6];
                    if (!r.read(gce, sizeof(gce))) {
                        break;
                    }
                    if (gce[0] == 4 && (gce[1] & 0x01U) != 0U) {
                        transparent = gce[4];
                    }
                    if (gce[5] != 0 && !skip_sub_blocks(r)) {
                        break;
                    }
                    continue;
                }
                if (!skip_sub_blocks(r)) {
                    break;
                }
                continue;
            }
            if (block != 0x2C) {
                break; // garbage: stop scanning
            }
            std::uint8_t desc[9];
            if (!r.read(desc, sizeof(desc))) {
                break;
            }
            ++frames;
            const std::uint32_t fx = le16(desc);
            const std::uint32_t fy = le16(desc + 2);
            const std::uint32_t fw = le16(desc + 4);
            const std::uint32_t fh = le16(desc + 6);
            const std::uint8_t fflags = desc[8];
            std::array<std::uint8_t, 768> local{};
            std::size_t local_count = 0;
            if ((fflags & 0x80U) != 0U) {
                local_count = std::size_t{2} << (fflags & 0x07U);
                if (!r.read(local.data(), local_count * 3U)) {
                    break;
                }
            }
            const int min_code = r.byte();
            if (min_code < 0) {
                break;
            }
            if (have_frame) {
                if (frames >= kMaxCountFrames || r.consumed() > kMaxCountBytes) {
                    frames = 0; // unknown
                    break;
                }
                if (!skip_sub_blocks(r)) {
                    break;
                }
                continue;
            }
            // First frame.
            if (canvas.empty()) {
                canvas = {fx + fw, fy + fh};
            }
            if (auto ok = check_source_size(canvas, "gif"); !ok) {
                return Unexpected<Error>(ok.error());
            }
            const PixelSize frame{fw, fh};
            if (canvas.pixels() > full_decode_limit(p) || frame.pixels() > full_decode_limit(p)) {
                return fail(ErrorCode::OutOfMemory, "gif: canvas too large: " + path.native());
            }
            if (min_code < 1 || min_code > 11) {
                return fail(ErrorCode::InvalidArgument, "gif: bad LZW code size: " + path.native());
            }
            const std::uint8_t* table = local_count > 0 ? local.data() : global.data();
            std::size_t table_count = local_count > 0 ? local_count : global_count;
            std::array<std::uint8_t, 768> gray{};
            if (table_count == 0) {
                for (std::size_t i = 0; i < 256; ++i) {
                    gray[i * 3] = gray[i * 3 + 1] = gray[i * 3 + 2] = static_cast<std::uint8_t>(i);
                }
                table = gray.data();
                table_count = 256;
            }
            auto cimg = make_image(canvas);
            if (!cimg) {
                return Unexpected<Error>(cimg.error());
            }
            canvas_img = std::move(*cimg);
            std::size_t written = 0;
            bool data_ended = false;
            if (!frame.empty()) {
                std::vector<std::uint8_t> indices(static_cast<std::size_t>(frame.pixels()), 0);
                written = lzw_decode(r, min_code, indices, data_ended);
                if (r.status() == -2) {
                    return fail(ErrorCode::Cancelled, "cancelled");
                }
                const bool interlaced = (fflags & 0x40U) != 0U;
                std::vector<std::uint32_t> row_of(fh);
                if (interlaced) {
                    std::uint32_t k = 0;
                    constexpr std::uint32_t kStart[] = {0, 4, 2, 1};
                    constexpr std::uint32_t kStep[] = {8, 8, 4, 2};
                    for (int pass = 0; pass < 4; ++pass) {
                        for (std::uint32_t y = kStart[pass]; y < fh; y += kStep[pass]) {
                            row_of[k++] = y;
                        }
                    }
                } else {
                    for (std::uint32_t y = 0; y < fh; ++y) {
                        row_of[y] = y;
                    }
                }
                for (std::size_t i = 0; i < written; ++i) {
                    const auto sy = static_cast<std::uint32_t>(i / fw);
                    const auto sx = static_cast<std::uint32_t>(i % fw);
                    const std::uint32_t cx = fx + sx;
                    const std::uint32_t cy = fy + row_of[sy];
                    if (cx >= canvas.width || cy >= canvas.height) {
                        continue;
                    }
                    const std::uint8_t idx = indices[i];
                    if (static_cast<int>(idx) == transparent || idx >= table_count) {
                        continue;
                    }
                    std::uint8_t* px = canvas_img.pixels.data() +
                                       std::size_t{cy} * canvas_img.stride + std::size_t{cx} * 4U;
                    px[0] = table[std::size_t{idx} * 3U];
                    px[1] = table[std::size_t{idx} * 3U + 1U];
                    px[2] = table[std::size_t{idx} * 3U + 2U];
                    px[3] = 255;
                }
            }
            if (written == 0 && !frame.empty()) {
                return fail(ErrorCode::InvalidArgument, "gif: no image data: " + path.native());
            }
            have_frame = true;
            out.info.has_alpha = transparent >= 0;
            if (!data_ended) {
                break; // truncated: partial first frame, stop scanning
            }
        }
        if (!have_frame) {
            return fail(ErrorCode::InvalidArgument, "gif: no image: " + path.native());
        }
        out.info.stored = canvas;
        out.info.display = canvas;
        out.info.frame_count = frames;
        out.info.animated = frames != 1;
        auto img = scale_and_orient(canvas_img.pixels.data(), canvas, canvas_img.stride, true,
                                    ExifOrientation::Normal, p.target);
        if (!img) {
            return Unexpected<Error>(img.error());
        }
        out.image = std::move(*img);
        return out;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "gif: allocation failed");
    }
}

} // namespace navigator::preview::detail
