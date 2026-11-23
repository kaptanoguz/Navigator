// navigator::mime: the name table of <navigator/mime.hpp>, entry classification and Type-column
// label keys. Pure, allocation-free, noexcept.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include <sys/stat.h>

#include <navigator/fs.hpp>
#include <navigator/mime.hpp>

namespace navigator::mime {

namespace {

struct Row {
    std::string_view ext; // lower case, without the dot; "tar.gz" style for compound rows
    std::string_view mime;
    std::string_view label;
};

// Mirrors the table in mime.hpp row by row (one Row per extension). Keep both in sync; tests
// check every row.
constexpr std::array kRows{
    Row{"png", "image/png", "type.image_png"},
    Row{"jpg", "image/jpeg", "type.image_jpeg"},
    Row{"jpeg", "image/jpeg", "type.image_jpeg"},
    Row{"jpe", "image/jpeg", "type.image_jpeg"},
    Row{"gif", "image/gif", "type.image_gif"},
    Row{"webp", "image/webp", "type.image_webp"},
    Row{"svg", "image/svg+xml", "type.image_svg"},
    Row{"bmp", "image/bmp", "type.image"},
    Row{"ico", "image/vnd.microsoft.icon", "type.image"},
    Row{"tif", "image/tiff", "type.image"},
    Row{"tiff", "image/tiff", "type.image"},
    Row{"avif", "image/avif", "type.image"},
    Row{"heic", "image/heif", "type.image"},
    Row{"heif", "image/heif", "type.image"},
    Row{"txt", "text/plain", "type.text"},
    Row{"log", "text/x-log", "type.text"},
    Row{"md", "text/markdown", "type.markdown"},
    Row{"markdown", "text/markdown", "type.markdown"},
    Row{"csv", "text/csv", "type.csv"},
    Row{"html", "text/html", "type.html"},
    Row{"htm", "text/html", "type.html"},
    Row{"css", "text/css", "type.source"},
    Row{"c", "text/x-csrc", "type.source"},
    Row{"h", "text/x-chdr", "type.source"},
    Row{"cpp", "text/x-c++src", "type.source"},
    Row{"cc", "text/x-c++src", "type.source"},
    Row{"cxx", "text/x-c++src", "type.source"},
    Row{"hpp", "text/x-c++hdr", "type.source"},
    Row{"hh", "text/x-c++hdr", "type.source"},
    Row{"hxx", "text/x-c++hdr", "type.source"},
    Row{"py", "text/x-python", "type.source"},
    Row{"rs", "text/rust", "type.source"},
    Row{"go", "text/x-go", "type.source"},
    Row{"java", "text/x-java", "type.source"},
    Row{"js", "text/javascript", "type.source"},
    Row{"mjs", "text/javascript", "type.source"},
    Row{"sh", "application/x-shellscript", "type.script"},
    Row{"bash", "application/x-shellscript", "type.script"},
    Row{"json", "application/json", "type.json"},
    Row{"xml", "application/xml", "type.xml"},
    Row{"yaml", "application/yaml", "type.text"},
    Row{"yml", "application/yaml", "type.text"},
    Row{"toml", "application/toml", "type.text"},
    Row{"pdf", "application/pdf", "type.pdf"},
    Row{"zip", "application/zip", "type.archive_zip"},
    Row{"tar", "application/x-tar", "type.archive"},
    Row{"tgz", "application/x-compressed-tar", "type.archive"},
    Row{"gz", "application/gzip", "type.archive"},
    Row{"bz2", "application/x-bzip2", "type.archive"},
    Row{"xz", "application/x-xz", "type.archive"},
    Row{"zst", "application/zstd", "type.archive"},
    Row{"7z", "application/x-7z-compressed", "type.archive"},
    Row{"rar", "application/vnd.rar", "type.archive"},
    Row{"deb", "application/vnd.debian.binary-package", "type.package_deb"},
    Row{"rpm", "application/x-rpm", "type.package"},
    Row{"appimage", "application/vnd.appimage", "type.appimage"},
    Row{"iso", "application/x-cd-image", "type.disk_image"},
    Row{"desktop", "application/x-desktop", "type.desktop_entry"},
    Row{"odt", "application/vnd.oasis.opendocument.text", "type.document"},
    Row{"doc", "application/msword", "type.document"},
    Row{"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
        "type.document"},
    Row{"ods", "application/vnd.oasis.opendocument.spreadsheet", "type.spreadsheet"},
    Row{"xls", "application/vnd.ms-excel", "type.spreadsheet"},
    Row{"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
        "type.spreadsheet"},
    Row{"odp", "application/vnd.oasis.opendocument.presentation", "type.presentation"},
    Row{"ppt", "application/vnd.ms-powerpoint", "type.presentation"},
    Row{"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation",
        "type.presentation"},
    Row{"mp3", "audio/mpeg", "type.audio"},
    Row{"flac", "audio/flac", "type.audio"},
    Row{"ogg", "audio/ogg", "type.audio"},
    Row{"oga", "audio/ogg", "type.audio"},
    Row{"opus", "audio/x-opus+ogg", "type.audio"},
    Row{"wav", "audio/x-wav", "type.audio"},
    Row{"m4a", "audio/mp4", "type.audio"},
    Row{"mp4", "video/mp4", "type.video"},
    Row{"m4v", "video/mp4", "type.video"},
    Row{"mkv", "video/x-matroska", "type.video"},
    Row{"webm", "video/webm", "type.video"},
    Row{"avi", "video/x-msvideo", "type.video"},
    Row{"mov", "video/quicktime", "type.video"},
    Row{"3gp", "video/3gpp", "type.video"},
    Row{"mts", "video/mp2t", "type.video"},
    Row{"m2ts", "video/mp2t", "type.video"},
    Row{"mpg", "video/mpeg", "type.video"},
    Row{"mpeg", "video/mpeg", "type.video"},
    Row{"ogv", "video/ogg", "type.video"},
    Row{"wmv", "video/x-ms-wmv", "type.video"},
    Row{"flv", "video/x-flv", "type.video"},
    Row{"ttf", "font/ttf", "type.font"},
    Row{"otf", "font/otf", "type.font"},
    Row{"woff2", "font/woff2", "type.font"},
};

// Compound suffixes, matched on "<previous part>.<extension>" when the previous part is "tar".
constexpr std::array kTarRows{
    Row{"gz", "application/x-compressed-tar", "type.archive"},
    Row{"bz2", "application/x-bzip-compressed-tar", "type.archive"},
    Row{"xz", "application/x-xz-compressed-tar", "type.archive"},
    Row{"zst", "application/x-zstd-compressed-tar", "type.archive"},
};

// Inode / executable values: (mime, label).
constexpr std::array kSpecial{
    Row{"", kDirectory, "type.folder"},
    Row{"", kSymlink, "type.link"},
    Row{"", kFifo, "type.fifo"},
    Row{"", kSocket, "type.socket"},
    Row{"", kBlockDevice, "type.block_device"},
    Row{"", kCharDevice, "type.char_device"},
    Row{"", kExecutable, "type.executable"},
};

constexpr char ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// `lower` must already be lower case.
constexpr bool iequals(std::string_view text, std::string_view lower) noexcept {
    if (text.size() != lower.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (ascii_lower(text[i]) != lower[i]) {
            return false;
        }
    }
    return true;
}

template <std::size_t N>
constexpr const Row* find_ext(const std::array<Row, N>& rows, std::string_view ext) noexcept {
    for (const Row& row : rows) {
        if (iequals(ext, row.ext)) {
            return &row;
        }
    }
    return nullptr;
}

// The dot-separated part right before the extension ("tar" in "a.tar.gz", also in "tar.gz").
constexpr std::string_view previous_part(std::string_view name, std::size_t ext_size) noexcept {
    const std::string_view head = name.substr(0, name.size() - ext_size - 1);
    const std::size_t dot = head.rfind('.');
    return dot == std::string_view::npos ? head : head.substr(dot + 1);
}

bool type_bits_are_link(std::uint32_t mode) noexcept { return (mode & S_IFMT) == S_IFLNK; }

std::string_view classify_other(std::uint32_t mode) noexcept {
    switch (mode & S_IFMT) {
    case S_IFIFO: return kFifo;
    case S_IFSOCK: return kSocket;
    case S_IFBLK: return kBlockDevice;
    case S_IFCHR: return kCharDevice;
    default: return kOctetStream;
    }
}

// `mode` may be the link's own (S_IFLNK, permissions meaningless) for a symlink listed without
// follow_symlinks: then the execute heuristic is skipped.
std::string_view classify_file(std::string_view name, std::uint32_t mode) noexcept {
    const std::string_view guess = guess_from_name(name);
    if (guess == kOctetStream && fs::extension(name).empty() && !type_bits_are_link(mode) &&
        (mode & 0111U) != 0) {
        return kExecutable;
    }
    return guess;
}

} // namespace

std::string_view guess_from_name(std::string_view name) noexcept {
    const std::string_view ext = fs::extension(name);
    if (ext.empty()) {
        return kOctetStream;
    }
    if (iequals(previous_part(name, ext.size()), "tar")) {
        if (const Row* row = find_ext(kTarRows, ext)) {
            return row->mime;
        }
    }
    if (const Row* row = find_ext(kRows, ext)) {
        return row->mime;
    }
    return kOctetStream;
}

std::string_view guess_for_entry(const fs::DirEntry& entry) noexcept {
    if (fs::is_directory_like(entry)) {
        return kDirectory;
    }
    switch (entry.kind) {
    case fs::EntryKind::Symlink:
        if (entry.dangling) {
            return kSymlink;
        }
        if (entry.target_kind == fs::EntryKind::File) {
            return classify_file(entry.name, entry.mode);
        }
        if (entry.target_kind == fs::EntryKind::Other) {
            return classify_other(entry.mode);
        }
        return kSymlink; // Unknown (loop, EACCES) or an impossible Symlink target kind
    case fs::EntryKind::Other: return classify_other(entry.mode);
    case fs::EntryKind::File: return classify_file(entry.name, entry.mode);
    case fs::EntryKind::Directory: return kDirectory; // handled above
    case fs::EntryKind::Unknown: break;
    }
    return guess_from_name(entry.name);
}

std::string_view type_label_key(std::string_view mime) noexcept {
    for (const Row& row : kSpecial) {
        if (row.mime == mime) {
            return row.label;
        }
    }
    for (const Row& row : kRows) {
        if (row.mime == mime) {
            return row.label;
        }
    }
    for (const Row& row : kTarRows) {
        if (row.mime == mime) {
            return row.label;
        }
    }
    constexpr std::array<std::pair<std::string_view, std::string_view>, 5> kPrefixes{{
        {"image/", "type.image"},
        {"audio/", "type.audio"},
        {"video/", "type.video"},
        {"text/", "type.text"},
        {"font/", "type.font"},
    }};
    for (const auto& [prefix, label] : kPrefixes) {
        if (mime.starts_with(prefix)) {
            return label;
        }
    }
    return "type.file";
}

} // namespace navigator::mime
