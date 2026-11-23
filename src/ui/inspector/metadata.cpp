// src/ui/inspector/metadata.cpp — Inspector metadata rows. See metadata.hpp.
#include "inspector/metadata.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <variant>

#include <navigator/i18n.hpp>

#include "browser/format.hpp"
#include "i18n/catalog.hpp"

namespace navigator::ui::inspector {

namespace {

std::string fixed(double v, int digits) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*f", digits, v);
    return buf;
}

std::string trimmed_decimal(double v, int max_digits) {
    std::string s = fixed(v, max_digits);
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') {
            s.pop_back();
        }
        if (!s.empty() && s.back() == '.') {
            s.pop_back();
        }
    }
    if (language() == Language::Turkish) {
        for (char& c : s) {
            if (c == '.') {
                c = ',';
            }
        }
    }
    return s;
}

void add(std::vector<MetaRow>& rows, std::string_view key, std::string value) {
    if (!value.empty()) {
        rows.push_back(MetaRow{std::string(tr(key)), std::move(value)});
    }
}

void push_group(std::vector<MetaGroup>& out, std::string_view key, std::vector<MetaRow> rows) {
    if (!rows.empty()) {
        out.push_back(MetaGroup{std::string(tr(key)), std::move(rows)});
    }
}

std::string yes_no(bool v) { return std::string(tr(v ? "meta.yes" : "meta.no")); }

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

std::string format_name(preview::ImageFormat f) {
    switch (f) {
    case preview::ImageFormat::Jpeg: return "JPEG";
    case preview::ImageFormat::Png: return "PNG";
    case preview::ImageFormat::WebP: return "WebP";
    case preview::ImageFormat::Gif: return "GIF";
    case preview::ImageFormat::Bmp: return "BMP";
    case preview::ImageFormat::Heif: return "HEIF";
    case preview::ImageFormat::Avif: return "AVIF";
    case preview::ImageFormat::Tiff: return "TIFF";
    case preview::ImageFormat::Svg: return "SVG";
    case preview::ImageFormat::Ico: return "ICO";
    case preview::ImageFormat::Unknown: break;
    }
    return {};
}

std::string orientation_text(preview::ExifOrientation o) {
    switch (o) {
    case preview::ExifOrientation::Normal: return {};
    case preview::ExifOrientation::Rotate90: return std::string(tr("meta.orientation.rotate90"));
    case preview::ExifOrientation::Rotate180: return std::string(tr("meta.orientation.rotate180"));
    case preview::ExifOrientation::Rotate270: return std::string(tr("meta.orientation.rotate270"));
    case preview::ExifOrientation::FlipHorizontal:
    case preview::ExifOrientation::FlipVertical:
    case preview::ExifOrientation::Transpose:
    case preview::ExifOrientation::Transverse: return std::string(tr("meta.orientation.mirrored"));
    }
    return {};
}

std::string encoding_name(preview::TextEncoding e) {
    switch (e) {
    case preview::TextEncoding::Utf8: return "UTF-8";
    case preview::TextEncoding::Utf8Bom: return "UTF-8 (BOM)";
    case preview::TextEncoding::Utf16Le: return "UTF-16 LE";
    case preview::TextEncoding::Utf16Be: return "UTF-16 BE";
    }
    return "UTF-8";
}

void image_groups(std::vector<MetaGroup>& out, const preview::ImageInfo& info) {
    std::vector<MetaRow> img;
    if (!info.display.empty()) {
        std::string dims = format_dimensions(info.display);
        if (info.display.pixels() >= 1'000'000U) {
            dims += "  ·  " + format_megapixels(info.display);
        }
        add(img, "meta.dimensions", std::move(dims));
    }
    add(img, "meta.format", format_name(info.format));
    if (info.bits_per_channel != 8 && info.bits_per_channel != 0) {
        add(img, "meta.bit_depth",
            substitute(tr("meta.bits_per_channel"), "n", std::to_string(info.bits_per_channel)));
    }
    if (info.has_alpha) {
        add(img, "meta.transparency", yes_no(true));
    }
    if (info.animated) {
        add(img, "meta.animated",
            info.frame_count > 1 ? tr_count("meta.frames", info.frame_count) : yes_no(true));
    }
    add(img, "meta.orientation", orientation_text(info.orientation));
    if (info.has_icc_profile) {
        add(img, "meta.color_profile", std::string(tr("meta.embedded")));
    }
    push_group(out, "meta.group.image", std::move(img));

    if (!info.exif) {
        return;
    }
    const preview::ExifInfo& x = *info.exif;
    std::vector<MetaRow> cam;
    std::string camera = x.model;
    if (!x.make.empty() && !x.model.starts_with(x.make)) {
        camera = x.model.empty() ? x.make : x.make + " " + x.model;
    }
    add(cam, "meta.camera", std::move(camera));
    add(cam, "meta.lens", x.lens_model);
    if (x.taken.known()) {
        add(cam, "meta.taken", format_exif_date(x.taken));
    }
    if (x.exposure_time && x.exposure_time->known()) {
        add(cam, "meta.exposure", format_exposure(*x.exposure_time));
    }
    if (x.f_number && *x.f_number > 0.0) {
        add(cam, "meta.aperture", format_aperture(*x.f_number));
    }
    if (x.iso) {
        add(cam, "meta.iso", std::to_string(*x.iso));
    }
    if (x.focal_length_mm && *x.focal_length_mm > 0.0) {
        add(cam, "meta.focal_length", format_focal(*x.focal_length_mm, x.focal_35mm));
    }
    if (x.flash_fired) {
        add(cam, "meta.flash",
            std::string(tr(*x.flash_fired ? "meta.flash_fired" : "meta.flash_off")));
    }
    add(cam, "meta.software", x.software);
    push_group(out, "meta.group.camera", std::move(cam));

    std::vector<MetaRow> gps;
    if (x.gps_latitude && x.gps_longitude) {
        add(gps, "meta.coordinates",
            format_coordinate(*x.gps_latitude, true) + ", " +
                format_coordinate(*x.gps_longitude, false));
    } else if (x.has_gps) {
        add(gps, "meta.coordinates", std::string(tr("meta.gps_present")));
    }
    if (x.gps_altitude_m) {
        add(gps, "meta.altitude", trimmed_decimal(*x.gps_altitude_m, 1) + " m");
    }
    push_group(out, "meta.group.location", std::move(gps));
}

void video_groups(std::vector<MetaGroup>& out, const preview::VideoInfo& v) {
    if (v.source == preview::MetadataSource::None) {
        return;
    }
    std::vector<MetaRow> vid;
    if (v.duration_ms) {
        add(vid, "meta.duration", format_duration(*v.duration_ms));
    }
    if (!v.frame.empty()) {
        add(vid, "meta.resolution", format_dimensions(v.frame));
    }
    if (v.frame_rate.known()) {
        add(vid, "meta.frame_rate", format_frame_rate(v.frame_rate));
    }
    add(vid, "meta.video_codec", upper(v.video_codec));
    add(vid, "meta.container", v.container_long.empty() ? v.container : v.container_long);
    if (v.bit_rate > 0) {
        add(vid, "meta.bit_rate", format_bit_rate(v.bit_rate));
    }
    if (v.rotation_degrees != 0) {
        add(vid, "meta.rotation", std::to_string(v.rotation_degrees) + "°");
    }
    if (v.subtitle_streams > 0) {
        add(vid, "meta.subtitles", format_count(v.subtitle_streams));
    }
    push_group(out, "meta.group.video", std::move(vid));

    std::vector<MetaRow> aud;
    if (!v.audio_codec.empty()) {
        add(aud, "meta.audio_codec", upper(v.audio_codec));
        if (v.audio_channels > 0) {
            const std::string ch = v.audio_channels == 1 ? std::string(tr("meta.mono"))
                                   : v.audio_channels == 2
                                       ? std::string(tr("meta.stereo"))
                                       : tr_count("meta.channels", v.audio_channels);
            add(aud, "meta.audio_channels", ch);
        }
        if (v.audio_sample_rate > 0) {
            add(aud, "meta.sample_rate", format_sample_rate(v.audio_sample_rate));
        }
        if (v.audio_streams > 1) {
            add(aud, "meta.audio_tracks", format_count(v.audio_streams));
        }
    }
    push_group(out, "meta.group.audio", std::move(aud));
}

} // namespace

std::string format_decimal(double value, int digits) { return trimmed_decimal(value, digits); }

std::string format_dimensions(preview::PixelSize size) {
    return std::to_string(size.width) + " × " + std::to_string(size.height);
}

std::string format_megapixels(preview::PixelSize size) {
    const double mp = static_cast<double>(size.pixels()) / 1.0e6;
    return trimmed_decimal(mp, mp >= 100.0 ? 0 : 1) + " MP";
}

std::string format_exposure(preview::Rational seconds) {
    if (!seconds.known() || seconds.num == 0) {
        return {};
    }
    const double s = static_cast<double>(seconds.num) / static_cast<double>(seconds.den);
    if (s >= 0.3) {
        return trimmed_decimal(s, 1) + " s";
    }
    const double inv = 1.0 / s;
    return "1/" + std::to_string(static_cast<long long>(std::llround(inv))) + " s";
}

std::string format_aperture(double f_number) { return "f/" + trimmed_decimal(f_number, 1); }

std::string format_focal(double mm, std::optional<std::uint32_t> mm35) {
    std::string out = trimmed_decimal(mm, 1) + " mm";
    if (mm35 && *mm35 > 0 && std::llround(mm) != static_cast<long long>(*mm35)) {
        out += "  ·  " + substitute(tr("meta.equivalent_35mm"), "n", std::to_string(*mm35));
    }
    return out;
}

std::string format_duration(std::int64_t ms) {
    if (ms < 0) {
        ms = 0;
    }
    const std::int64_t total = (ms + 500) / 1000;
    const std::int64_t h = total / 3600;
    const std::int64_t m = (total / 60) % 60;
    const std::int64_t s = total % 60;
    char buf[48];
    if (h > 0) {
        std::snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", static_cast<long long>(h),
                      static_cast<long long>(m), static_cast<long long>(s));
    } else {
        std::snprintf(buf, sizeof buf, "%lld:%02lld", static_cast<long long>(m),
                      static_cast<long long>(s));
    }
    return buf;
}

std::string format_frame_rate(preview::Rational rate) {
    if (!rate.known() || rate.num == 0) {
        return {};
    }
    const double fps = static_cast<double>(rate.num) / static_cast<double>(rate.den);
    return trimmed_decimal(fps, 2) + " fps";
}

std::string format_bit_rate(std::uint64_t bits_per_second) {
    const std::uint64_t bps = bits_per_second;
    if (bps >= 1'000'000U) {
        return trimmed_decimal(static_cast<double>(bps) / 1.0e6, 1) + " Mb/s";
    }
    return trimmed_decimal(static_cast<double>(bps) / 1.0e3, 0) + " kb/s";
}

std::string format_sample_rate(std::uint32_t hz) {
    return trimmed_decimal(static_cast<double>(hz) / 1000.0, 1) + " kHz";
}

std::string format_coordinate(double degrees, bool latitude) {
    const char* hemi = latitude ? (degrees < 0.0 ? "meta.south" : "meta.north")
                                : (degrees < 0.0 ? "meta.west" : "meta.east");
    std::string v = fixed(std::abs(degrees), 5);
    if (language() == Language::Turkish) {
        for (char& c : v) {
            if (c == '.') {
                c = ',';
            }
        }
    }
    return v + "° " + std::string(tr(hemi));
}

std::string format_exif_date(const preview::ExifDateTime& when) {
    if (!when.known()) {
        return {};
    }
    char buf[64];
    // ISO-like, unambiguous in both languages; the camera's local time.
    std::snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u", unsigned{when.year},
                  unsigned{when.month}, unsigned{when.day}, unsigned{when.hour},
                  unsigned{when.minute});
    std::string out = buf;
    if (when.utc_offset_minutes) {
        const int off = *when.utc_offset_minutes;
        std::snprintf(buf, sizeof buf, " UTC%c%02d:%02d", off < 0 ? '-' : '+', std::abs(off) / 60,
                      std::abs(off) % 60);
        out += buf;
    }
    return out;
}

std::string header_subtitle(const fs::DirEntry& entry, const preview::PreviewResult* result) {
    std::string out = type_label(entry);
    if (fs::is_directory_like(entry)) {
        if (result != nullptr) {
            if (const auto* f = std::get_if<preview::FolderPreview>(&result->content)) {
                out += "  ·  " + tr_count("inspector.items", f->total);
            }
        }
        return out;
    }
    if (!entry.metadata_error) {
        out += "  ·  " + format_size(entry.size);
    }
    return out;
}

std::vector<MetaGroup> build_metadata(const fs::DirEntry& entry,
                                      const preview::PreviewResult* result, Timestamp now) {
    std::vector<MetaGroup> out;
    if (result != nullptr) {
        if (const auto* img = std::get_if<preview::ImagePreview>(&result->content)) {
            image_groups(out, img->info);
        } else if (const auto* vid = std::get_if<preview::VideoPreview>(&result->content)) {
            video_groups(out, vid->info);
        } else if (const auto* txt = std::get_if<preview::TextPreview>(&result->content)) {
            std::vector<MetaRow> rows;
            add(rows, "meta.encoding", encoding_name(txt->encoding));
            add(rows, "meta.lines",
                format_count(txt->line_count) + (txt->truncated ? "+" : std::string()));
            push_group(out, "meta.group.text", std::move(rows));
        } else if (const auto* dir = std::get_if<preview::FolderPreview>(&result->content)) {
            std::vector<MetaRow> rows;
            add(rows, "meta.folders", format_count(dir->folders));
            add(rows, "meta.files", format_count(dir->files));
            if (dir->hidden > 0) {
                add(rows, "meta.hidden_items", format_count(dir->hidden));
            }
            push_group(out, "meta.group.contents", std::move(rows));
        }
    }
    std::vector<MetaRow> file;
    add(file, "meta.kind", type_label(entry));
    if (!fs::is_directory_like(entry) && !entry.metadata_error) {
        std::string size = format_size(entry.size);
        if (entry.size >= 1000U) {
            size += "  ·  " + substitute(tr("meta.bytes"), "n", format_count(entry.size));
        }
        add(file, "meta.size", std::move(size));
    }
    if (entry.mtime != Timestamp{}) {
        add(file, "meta.modified", format_date(entry.mtime, now));
    }
    if (entry.kind == fs::EntryKind::Symlink) {
        add(file, "meta.link",
            std::string(tr(entry.dangling ? "meta.link_broken" : "meta.link_yes")));
    }
    if (entry.mode != 0) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%03o", entry.mode & 0777U);
        add(file, "meta.permissions", buf);
    }
    // File first: the facts every item has; then the content groups.
    out.insert(out.begin(), MetaGroup{std::string(tr("meta.group.file")), std::move(file)});
    if (out.front().rows.empty()) {
        out.erase(out.begin());
    }
    return out;
}

} // namespace navigator::ui::inspector
