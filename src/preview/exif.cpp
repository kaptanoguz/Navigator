// src/preview/exif.cpp: EXIF block -> ExifInfo through libexif (bounds-checked by libexif; every
// entry's format, component count and size are checked again here before reading).
#include <cmath>
#include <cstring>
#include <memory>
#include <new>

#include <libexif/exif-data.h>
#include <libexif/exif-utils.h>

#include "internal.hpp"

namespace navigator::preview::detail {

namespace {

constexpr std::size_t kMaxExifBytes = std::size_t{4} << 20U;
constexpr std::size_t kMaxExifString = 256;

struct ExifDataDeleter {
    void operator()(ExifData* d) const noexcept { exif_data_unref(d); }
};

[[nodiscard]] std::size_t format_size(ExifFormat f) noexcept {
    return static_cast<std::size_t>(exif_format_get_size(f));
}

[[nodiscard]] bool entry_ok(const ExifEntry* e, std::size_t components) noexcept {
    if (e == nullptr || e->data == nullptr || e->components < components) {
        return false;
    }
    const std::size_t fs = format_size(e->format);
    return fs != 0 && e->size >= fs * components;
}

[[nodiscard]] std::optional<std::uint32_t> get_uint(const ExifEntry* e, ExifByteOrder order,
                                                    std::size_t index = 0) noexcept {
    if (!entry_ok(e, index + 1)) {
        return std::nullopt;
    }
    const unsigned char* p = e->data + format_size(e->format) * index;
    switch (e->format) {
    case EXIF_FORMAT_BYTE:
    case EXIF_FORMAT_UNDEFINED: return std::uint32_t{p[0]};
    case EXIF_FORMAT_SHORT: return std::uint32_t{exif_get_short(p, order)};
    case EXIF_FORMAT_LONG: return std::uint32_t{exif_get_long(p, order)};
    case EXIF_FORMAT_SSHORT: {
        const ExifSShort v = exif_get_sshort(p, order);
        return v < 0 ? std::nullopt : std::optional<std::uint32_t>(static_cast<std::uint32_t>(v));
    }
    case EXIF_FORMAT_SLONG: {
        const ExifSLong v = exif_get_slong(p, order);
        return v < 0 ? std::nullopt : std::optional<std::uint32_t>(static_cast<std::uint32_t>(v));
    }
    default: return std::nullopt;
    }
}

[[nodiscard]] std::optional<Rational> get_rational(const ExifEntry* e, ExifByteOrder order,
                                                   std::size_t index = 0) noexcept {
    if (!entry_ok(e, index + 1)) {
        return std::nullopt;
    }
    const unsigned char* p = e->data + format_size(e->format) * index;
    if (e->format == EXIF_FORMAT_RATIONAL) {
        const ExifRational r = exif_get_rational(p, order);
        return Rational{r.numerator, r.denominator};
    }
    if (e->format == EXIF_FORMAT_SRATIONAL) {
        const ExifSRational r = exif_get_srational(p, order);
        if (r.numerator < 0 || r.denominator < 0) {
            return std::nullopt;
        }
        return Rational{static_cast<std::uint32_t>(r.numerator),
                        static_cast<std::uint32_t>(r.denominator)};
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<double> get_real(const ExifEntry* e, ExifByteOrder order,
                                             std::size_t index = 0) noexcept {
    const auto r = get_rational(e, order, index);
    if (!r || r->den == 0) {
        return std::nullopt;
    }
    return static_cast<double>(r->num) / static_cast<double>(r->den);
}

[[nodiscard]] std::string get_string(const ExifEntry* e) {
    if (e == nullptr || e->data == nullptr || e->size == 0) {
        return {};
    }
    if (e->format != EXIF_FORMAT_ASCII && e->format != EXIF_FORMAT_UNDEFINED &&
        e->format != EXIF_FORMAT_BYTE) {
        return {};
    }
    return clean_exif_string(std::string_view(reinterpret_cast<const char*>(e->data), e->size));
}

[[nodiscard]] const ExifEntry* entry(ExifData* d, ExifIfd ifd, unsigned tag) noexcept {
    if (d->ifd[ifd] == nullptr) {
        return nullptr;
    }
    return exif_content_get_entry(d->ifd[ifd], static_cast<ExifTag>(tag));
}

[[nodiscard]] int digits(std::string_view s, std::size_t at, std::size_t n) noexcept {
    if (at + n > s.size()) {
        return -1;
    }
    int v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const char c = s[at + i];
        if (c < '0' || c > '9') {
            return -1;
        }
        v = v * 10 + (c - '0');
    }
    return v;
}

[[nodiscard]] ExifDateTime parse_datetime(std::string_view s) noexcept {
    ExifDateTime dt;
    const int y = digits(s, 0, 4);
    const int mo = digits(s, 5, 2);
    const int d = digits(s, 8, 2);
    const int h = digits(s, 11, 2);
    const int mi = digits(s, 14, 2);
    const int se = digits(s, 17, 2);
    if (y <= 0 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 ||
        se < 0 || se > 60) {
        return dt;
    }
    dt.year = static_cast<std::uint16_t>(y);
    dt.month = static_cast<std::uint8_t>(mo);
    dt.day = static_cast<std::uint8_t>(d);
    dt.hour = static_cast<std::uint8_t>(h);
    dt.minute = static_cast<std::uint8_t>(mi);
    dt.second = static_cast<std::uint8_t>(se);
    return dt;
}

[[nodiscard]] std::optional<std::int16_t> parse_offset(std::string_view s) noexcept {
    if (s.size() < 6 || (s[0] != '+' && s[0] != '-') || s[3] != ':') {
        return std::nullopt;
    }
    const int h = digits(s, 1, 2);
    const int m = digits(s, 4, 2);
    if (h < 0 || h > 14 || m < 0 || m > 59) {
        return std::nullopt;
    }
    const int total = h * 60 + m;
    return static_cast<std::int16_t>(s[0] == '-' ? -total : total);
}

[[nodiscard]] std::optional<double> gps_coord(ExifData* d, ExifByteOrder order, unsigned tag,
                                              unsigned ref_tag, char negative) {
    const ExifEntry* e = entry(d, EXIF_IFD_GPS, tag);
    const auto deg = get_real(e, order, 0);
    const auto min = get_real(e, order, 1);
    const auto sec = get_real(e, order, 2);
    if (!deg || !min || !sec) {
        return std::nullopt;
    }
    double v = *deg + *min / 60.0 + *sec / 3600.0;
    if (!std::isfinite(v)) {
        return std::nullopt;
    }
    const std::string ref = get_string(entry(d, EXIF_IFD_GPS, ref_tag));
    if (!ref.empty() && (ref[0] == negative || ref[0] == negative + ('a' - 'A'))) {
        v = -v;
    }
    return v;
}

} // namespace

std::string clean_exif_string(std::string_view raw) {
    // Cut at the first NUL, trim spaces.
    if (const std::size_t nul = raw.find('\0'); nul != std::string_view::npos) {
        raw = raw.substr(0, nul);
    }
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t')) {
        raw.remove_suffix(1);
    }
    while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t')) {
        raw.remove_prefix(1);
    }
    std::string out;
    out.reserve(std::min(raw.size(), kMaxExifString));
    std::size_t i = 0;
    while (i < raw.size()) {
        const auto b = static_cast<std::uint8_t>(raw[i]);
        std::size_t len = 0;
        if (b < 0x80U) {
            len = 1;
        } else if (b >= 0xC2U && b <= 0xDFU) {
            len = 2;
        } else if (b >= 0xE0U && b <= 0xEFU) {
            len = 3;
        } else if (b >= 0xF0U && b <= 0xF4U) {
            len = 4;
        }
        bool valid = len > 0 && i + len <= raw.size();
        for (std::size_t k = 1; valid && k < len; ++k) {
            valid = (static_cast<std::uint8_t>(raw[i + k]) & 0xC0U) == 0x80U;
        }
        if (valid && len == 3) {
            const auto b1 = static_cast<std::uint8_t>(raw[i + 1]);
            valid = (b != 0xE0U || b1 >= 0xA0U) && (b != 0xEDU || b1 <= 0x9FU);
        } else if (valid && len == 4) {
            const auto b1 = static_cast<std::uint8_t>(raw[i + 1]);
            valid = (b != 0xF0U || b1 >= 0x90U) && (b != 0xF4U || b1 <= 0x8FU);
        }
        const std::size_t add = valid ? len : 3;
        if (out.size() + add > kMaxExifString) {
            break;
        }
        if (valid) {
            out.append(raw.substr(i, len));
            i += len;
        } else {
            append_utf8(out, 0xFFFD);
            ++i;
        }
    }
    return out;
}

std::optional<ExifInfo> parse_exif(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < 8 || size > kMaxExifBytes) {
        return std::nullopt;
    }
    try {
        std::vector<std::uint8_t> buf;
        if (std::memcmp(data, "Exif\0\0", 6) != 0) {
            const bool tiff =
                (data[0] == 'I' && data[1] == 'I') || (data[0] == 'M' && data[1] == 'M');
            if (!tiff) {
                return std::nullopt;
            }
            buf.reserve(size + 6);
            buf.insert(buf.end(), {'E', 'x', 'i', 'f', 0, 0});
            buf.insert(buf.end(), data, data + size);
            data = buf.data();
            size = buf.size();
        }
        std::unique_ptr<ExifData, ExifDataDeleter> d(exif_data_new());
        if (!d) {
            return std::nullopt;
        }
        exif_data_unset_option(d.get(), EXIF_DATA_OPTION_IGNORE_UNKNOWN_TAGS);
        exif_data_unset_option(d.get(), EXIF_DATA_OPTION_FOLLOW_SPECIFICATION);
        exif_data_load_data(d.get(), data, static_cast<unsigned int>(size));
        ExifData* ed = d.get();
        bool any = false;
        for (const ExifContent* content : ed->ifd) {
            if (content != nullptr && content->count > 0) {
                any = true;
            }
        }
        if (!any) {
            return std::nullopt;
        }
        const ExifByteOrder order = exif_data_get_byte_order(ed);
        ExifInfo info;
        if (const auto o = get_uint(entry(ed, EXIF_IFD_0, 0x0112), order)) {
            info.orientation = orientation_from_tag(*o);
        }
        info.make = get_string(entry(ed, EXIF_IFD_0, 0x010F));
        info.model = get_string(entry(ed, EXIF_IFD_0, 0x0110));
        info.software = get_string(entry(ed, EXIF_IFD_0, 0x0131));
        info.lens_model = get_string(entry(ed, EXIF_IFD_EXIF, 0xA434));
        struct DateTag {
            ExifIfd ifd;
            unsigned tag;
            unsigned offset_tag;
        };
        constexpr DateTag kDates[] = {{EXIF_IFD_EXIF, 0x9003, 0x9011},
                                      {EXIF_IFD_EXIF, 0x9004, 0x9012},
                                      {EXIF_IFD_0, 0x0132, 0x9010}};
        for (const DateTag& dt : kDates) {
            const std::string s = get_string(entry(ed, dt.ifd, dt.tag));
            ExifDateTime v = parse_datetime(s);
            if (v.known()) {
                v.utc_offset_minutes =
                    parse_offset(get_string(entry(ed, EXIF_IFD_EXIF, dt.offset_tag)));
                info.taken = v;
                break;
            }
        }
        if (const auto exposure = get_rational(entry(ed, EXIF_IFD_EXIF, 0x829A), order);
            exposure && exposure->known()) {
            info.exposure_time = exposure;
        }
        info.f_number = get_real(entry(ed, EXIF_IFD_EXIF, 0x829D), order);
        info.iso = get_uint(entry(ed, EXIF_IFD_EXIF, 0x8827), order);
        info.focal_length_mm = get_real(entry(ed, EXIF_IFD_EXIF, 0x920A), order);
        info.focal_35mm = get_uint(entry(ed, EXIF_IFD_EXIF, 0xA405), order);
        if (const auto flash = get_uint(entry(ed, EXIF_IFD_EXIF, 0x9209), order)) {
            info.flash_fired = (*flash & 1U) != 0U;
        }
        info.has_gps = ed->ifd[EXIF_IFD_GPS] != nullptr && ed->ifd[EXIF_IFD_GPS]->count > 0;
        if (info.has_gps) {
            info.gps_latitude = gps_coord(ed, order, 0x0002, 0x0001, 'S');
            info.gps_longitude = gps_coord(ed, order, 0x0004, 0x0003, 'W');
            if (auto alt = get_real(entry(ed, EXIF_IFD_GPS, 0x0006), order)) {
                const auto ref = get_uint(entry(ed, EXIF_IFD_GPS, 0x0005), order);
                if (ref && *ref == 1U) {
                    *alt = -*alt;
                }
                info.gps_altitude_m = alt;
            }
        }
        return info;
    } catch (const std::bad_alloc&) {
        return std::nullopt;
    }
}

} // namespace navigator::preview::detail
