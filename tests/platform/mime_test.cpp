// navigator::mime: every row of the mime.hpp name table (the test contract), compound tar
// suffixes, entry classification and Type-column label keys.
#include <cstdint>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <sys/stat.h>

#include <navigator/fs.hpp>
#include <navigator/mime.hpp>

namespace {

namespace mime = navigator::mime;
using navigator::fs::DirEntry;
using navigator::fs::EntryKind;

struct Row {
    std::string_view ext;
    std::string_view mime;
    std::string_view label;
};

#define OOXML "application/vnd.openxmlformats-officedocument"

// Transcribed from include/navigator/mime.hpp, independently of src/platform/mime.cpp.
constexpr Row kTable[] = {
    {"png", "image/png", "type.image_png"},
    {"jpg", "image/jpeg", "type.image_jpeg"},
    {"jpeg", "image/jpeg", "type.image_jpeg"},
    {"jpe", "image/jpeg", "type.image_jpeg"},
    {"gif", "image/gif", "type.image_gif"},
    {"webp", "image/webp", "type.image_webp"},
    {"svg", "image/svg+xml", "type.image_svg"},
    {"bmp", "image/bmp", "type.image"},
    {"ico", "image/vnd.microsoft.icon", "type.image"},
    {"tif", "image/tiff", "type.image"},
    {"tiff", "image/tiff", "type.image"},
    {"avif", "image/avif", "type.image"},
    {"heic", "image/heif", "type.image"},
    {"heif", "image/heif", "type.image"},
    {"txt", "text/plain", "type.text"},
    {"log", "text/x-log", "type.text"},
    {"md", "text/markdown", "type.markdown"},
    {"markdown", "text/markdown", "type.markdown"},
    {"csv", "text/csv", "type.csv"},
    {"html", "text/html", "type.html"},
    {"htm", "text/html", "type.html"},
    {"css", "text/css", "type.source"},
    {"c", "text/x-csrc", "type.source"},
    {"h", "text/x-chdr", "type.source"},
    {"cpp", "text/x-c++src", "type.source"},
    {"cc", "text/x-c++src", "type.source"},
    {"cxx", "text/x-c++src", "type.source"},
    {"hpp", "text/x-c++hdr", "type.source"},
    {"hh", "text/x-c++hdr", "type.source"},
    {"hxx", "text/x-c++hdr", "type.source"},
    {"py", "text/x-python", "type.source"},
    {"rs", "text/rust", "type.source"},
    {"go", "text/x-go", "type.source"},
    {"java", "text/x-java", "type.source"},
    {"js", "text/javascript", "type.source"},
    {"mjs", "text/javascript", "type.source"},
    {"sh", "application/x-shellscript", "type.script"},
    {"bash", "application/x-shellscript", "type.script"},
    {"json", "application/json", "type.json"},
    {"xml", "application/xml", "type.xml"},
    {"yaml", "application/yaml", "type.text"},
    {"yml", "application/yaml", "type.text"},
    {"toml", "application/toml", "type.text"},
    {"pdf", "application/pdf", "type.pdf"},
    {"zip", "application/zip", "type.archive_zip"},
    {"tar", "application/x-tar", "type.archive"},
    {"tar.gz", "application/x-compressed-tar", "type.archive"},
    {"tgz", "application/x-compressed-tar", "type.archive"},
    {"tar.bz2", "application/x-bzip-compressed-tar", "type.archive"},
    {"tar.xz", "application/x-xz-compressed-tar", "type.archive"},
    {"tar.zst", "application/x-zstd-compressed-tar", "type.archive"},
    {"gz", "application/gzip", "type.archive"},
    {"bz2", "application/x-bzip2", "type.archive"},
    {"xz", "application/x-xz", "type.archive"},
    {"zst", "application/zstd", "type.archive"},
    {"7z", "application/x-7z-compressed", "type.archive"},
    {"rar", "application/vnd.rar", "type.archive"},
    {"deb", "application/vnd.debian.binary-package", "type.package_deb"},
    {"rpm", "application/x-rpm", "type.package"},
    {"appimage", "application/vnd.appimage", "type.appimage"},
    {"iso", "application/x-cd-image", "type.disk_image"},
    {"desktop", "application/x-desktop", "type.desktop_entry"},
    {"odt", "application/vnd.oasis.opendocument.text", "type.document"},
    {"doc", "application/msword", "type.document"},
    {"docx", OOXML ".wordprocessingml.document", "type.document"},
    {"ods", "application/vnd.oasis.opendocument.spreadsheet", "type.spreadsheet"},
    {"xls", "application/vnd.ms-excel", "type.spreadsheet"},
    {"xlsx", OOXML ".spreadsheetml.sheet", "type.spreadsheet"},
    {"odp", "application/vnd.oasis.opendocument.presentation", "type.presentation"},
    {"ppt", "application/vnd.ms-powerpoint", "type.presentation"},
    {"pptx", OOXML ".presentationml.presentation", "type.presentation"},
    {"mp3", "audio/mpeg", "type.audio"},
    {"flac", "audio/flac", "type.audio"},
    {"ogg", "audio/ogg", "type.audio"},
    {"oga", "audio/ogg", "type.audio"},
    {"opus", "audio/x-opus+ogg", "type.audio"},
    {"wav", "audio/x-wav", "type.audio"},
    {"m4a", "audio/mp4", "type.audio"},
    {"mp4", "video/mp4", "type.video"},
    {"m4v", "video/mp4", "type.video"},
    {"mkv", "video/x-matroska", "type.video"},
    {"webm", "video/webm", "type.video"},
    {"avi", "video/x-msvideo", "type.video"},
    {"mov", "video/quicktime", "type.video"},
    {"ttf", "font/ttf", "type.font"},
    {"otf", "font/otf", "type.font"},
    {"woff2", "font/woff2", "type.font"},
};

#undef OOXML

std::string upper(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

// "jpeg" -> "JpEg": alternating case.
std::string mixed(std::string_view text) {
    std::string out(text);
    for (std::size_t i = 0; i < out.size(); i += 2) {
        if (out[i] >= 'a' && out[i] <= 'z') {
            out[i] = static_cast<char>(out[i] - 'a' + 'A');
        }
    }
    return out;
}

DirEntry entry(std::string name, EntryKind kind, std::uint32_t mode) {
    DirEntry e;
    e.name = std::move(name);
    e.kind = kind;
    e.mode = mode;
    return e;
}

} // namespace

TEST_CASE("mime: every row of the name table", "[mime]") {
    for (const Row& row : kTable) {
        INFO("extension " << row.ext);
        const std::string name = "file." + std::string(row.ext);
        CHECK(mime::guess_from_name(name) == row.mime);
        CHECK(mime::guess_from_name(upper(name)) == row.mime);
        CHECK(mime::guess_from_name(mixed(name)) == row.mime);
        CHECK(mime::guess_from_name("some.dots.in." + std::string(row.ext)) == row.mime);
        CHECK(mime::type_label_key(row.mime) == row.label);
    }
}

TEST_CASE("mime: compound tar suffixes win over the last extension", "[mime]") {
    CHECK(mime::guess_from_name("backup.tar.gz") == "application/x-compressed-tar");
    CHECK(mime::guess_from_name("BACKUP.TAR.GZ") == "application/x-compressed-tar");
    CHECK(mime::guess_from_name("a.Tar.Bz2") == "application/x-bzip-compressed-tar");
    CHECK(mime::guess_from_name("v1.2.tar.xz") == "application/x-xz-compressed-tar");
    CHECK(mime::guess_from_name("x.tar.zst") == "application/x-zstd-compressed-tar");
    CHECK(mime::guess_from_name("tar.gz") == "application/x-compressed-tar");
    // Only "tar" as the whole previous part counts.
    CHECK(mime::guess_from_name("star.gz") == "application/gzip");
    CHECK(mime::guess_from_name("a.tarx.gz") == "application/gzip");
    CHECK(mime::guess_from_name("a.tar.gz.part") == mime::kOctetStream);
    // tar.<other>: the last extension decides.
    CHECK(mime::guess_from_name("a.tar.zip") == "application/zip");
    CHECK(mime::guess_from_name("a.tar.lz") == mime::kOctetStream);
    CHECK(mime::guess_from_name("a.gz.tar") == "application/x-tar");
}

TEST_CASE("mime: names outside the table", "[mime]") {
    CHECK(mime::guess_from_name("") == mime::kOctetStream);
    CHECK(mime::guess_from_name("README") == mime::kOctetStream);
    CHECK(mime::guess_from_name(".png") == mime::kOctetStream); // hidden file, no extension
    CHECK(mime::guess_from_name(".bashrc") == mime::kOctetStream);
    CHECK(mime::guess_from_name("trailing.") == mime::kOctetStream);
    CHECK(mime::guess_from_name("video.ts") == mime::kOctetStream); // ambiguous, left unknown
    CHECK(mime::guess_from_name("x.pngx") == mime::kOctetStream);
    CHECK(mime::guess_from_name("x.pn") == mime::kOctetStream);
    CHECK(mime::guess_from_name("x.verylongextension") == mime::kOctetStream);
    CHECK(mime::guess_from_name(".hidden.png") == "image/png");
    // Non-ASCII bytes never fold onto ASCII.
    CHECK(mime::guess_from_name("x.p\xC3\xB1g") == mime::kOctetStream);
}

TEST_CASE("mime: guess_for_entry classifies inode types, links and executables", "[mime]") {
    CHECK(mime::guess_for_entry(entry("photos.png", EntryKind::Directory, S_IFDIR | 0755)) ==
          mime::kDirectory);
    CHECK(mime::guess_for_entry(entry("a.png", EntryKind::File, S_IFREG | 0644)) == "image/png");

    // Executables: only a name without an extension that the table does not know.
    CHECK(mime::guess_for_entry(entry("run", EntryKind::File, S_IFREG | 0755)) ==
          mime::kExecutable);
    CHECK(mime::guess_for_entry(entry("run", EntryKind::File, S_IFREG | 0001)) ==
          mime::kExecutable);
    CHECK(mime::guess_for_entry(entry("run", EntryKind::File, S_IFREG | 0644)) ==
          mime::kOctetStream);
    CHECK(mime::guess_for_entry(entry("build.sh", EntryKind::File, S_IFREG | 0755)) ==
          "application/x-shellscript");
    CHECK(mime::guess_for_entry(entry("tool.bin", EntryKind::File, S_IFREG | 0755)) ==
          mime::kOctetStream);
    CHECK(mime::guess_for_entry(entry(".local-tool", EntryKind::File, S_IFREG | 0755)) ==
          mime::kExecutable);

    // Other: by mode type bits.
    CHECK(mime::guess_for_entry(entry("p", EntryKind::Other, S_IFIFO | 0600)) == mime::kFifo);
    CHECK(mime::guess_for_entry(entry("s", EntryKind::Other, S_IFSOCK | 0600)) == mime::kSocket);
    CHECK(mime::guess_for_entry(entry("sda", EntryKind::Other, S_IFBLK | 0660)) ==
          mime::kBlockDevice);
    CHECK(mime::guess_for_entry(entry("tty", EntryKind::Other, S_IFCHR | 0620)) ==
          mime::kCharDevice);
    CHECK(mime::guess_for_entry(entry("x.png", EntryKind::Other, 0)) == mime::kOctetStream);

    // Symlinks.
    DirEntry link = entry("shortcut.txt", EntryKind::Symlink, S_IFLNK | 0777);
    link.target_kind = EntryKind::Directory;
    CHECK(mime::guess_for_entry(link) == mime::kDirectory);
    link.target_kind = EntryKind::File;
    CHECK(mime::guess_for_entry(link) == "text/plain"); // the link's name decides
    link.target_kind = EntryKind::Unknown;              // loop or EACCES
    CHECK(mime::guess_for_entry(link) == mime::kSymlink);
    link.target_kind = EntryKind::File;
    link.dangling = true;
    CHECK(mime::guess_for_entry(link) == mime::kSymlink);
    DirEntry dangling = entry("gone.png", EntryKind::Symlink, S_IFLNK | 0777);
    dangling.dangling = true;
    CHECK(mime::guess_for_entry(dangling) == mime::kSymlink);

    // Link to a file without extension: the link's own 0777 says nothing about the target ...
    DirEntry exe_link = entry("tool", EntryKind::Symlink, S_IFLNK | 0777);
    exe_link.target_kind = EntryKind::File;
    CHECK(mime::guess_for_entry(exe_link) == mime::kOctetStream);
    // ... but target metadata (follow_symlinks, or mime_type()) does.
    exe_link.mode = S_IFREG | 0755;
    CHECK(mime::guess_for_entry(exe_link) == mime::kExecutable);
    exe_link.mode = S_IFREG | 0644;
    CHECK(mime::guess_for_entry(exe_link) == mime::kOctetStream);

    // Link to Other: by the mode type bits when they describe the target.
    DirEntry fifo_link = entry("pipe-link", EntryKind::Symlink, S_IFIFO | 0600);
    fifo_link.target_kind = EntryKind::Other;
    CHECK(mime::guess_for_entry(fifo_link) == mime::kFifo);
    fifo_link.mode = S_IFLNK | 0777;
    CHECK(mime::guess_for_entry(fifo_link) == mime::kOctetStream);

    // Unknown kind (metadata_error / DT_UNKNOWN): the name only, never "executable".
    DirEntry unknown = entry("song.MP3", EntryKind::Unknown, 0);
    unknown.metadata_error = navigator::ErrorCode::PermissionDenied;
    CHECK(mime::guess_for_entry(unknown) == "audio/mpeg");
    CHECK(mime::guess_for_entry(entry("run", EntryKind::Unknown, 0755)) == mime::kOctetStream);
}

TEST_CASE("mime: type_label_key for inode values and fallbacks", "[mime]") {
    CHECK(mime::type_label_key(mime::kDirectory) == "type.folder");
    CHECK(mime::type_label_key(mime::kSymlink) == "type.link");
    CHECK(mime::type_label_key(mime::kFifo) == "type.fifo");
    CHECK(mime::type_label_key(mime::kSocket) == "type.socket");
    CHECK(mime::type_label_key(mime::kBlockDevice) == "type.block_device");
    CHECK(mime::type_label_key(mime::kCharDevice) == "type.char_device");
    CHECK(mime::type_label_key(mime::kExecutable) == "type.executable");
    CHECK(mime::type_label_key(mime::kOctetStream) == "type.file");

    // By top-level type for MIME strings outside the table.
    CHECK(mime::type_label_key("image/x-xcf") == "type.image");
    CHECK(mime::type_label_key("audio/x-aiff") == "type.audio");
    CHECK(mime::type_label_key("video/mpeg") == "type.video");
    CHECK(mime::type_label_key("text/x-tex") == "type.text");
    CHECK(mime::type_label_key("font/woff") == "type.font");
    CHECK(mime::type_label_key("application/x-unknown") == "type.file");
    CHECK(mime::type_label_key("") == "type.file");
    CHECK(mime::type_label_key("image") == "type.file"); // no slash: no prefix match
    CHECK(mime::type_label_key("inode/other") == "type.file");
    CHECK(mime::type_label_key("IMAGE/PNG") == "type.file"); // MIME strings are lower case
}

TEST_CASE("mime: results have static storage and are usable at any time", "[mime]") {
    std::string_view kept;
    {
        const std::string temp = "transient.json";
        kept = mime::guess_from_name(temp);
    }
    CHECK(kept == "application/json");
    CHECK(mime::type_label_key(kept) == "type.json");
}
