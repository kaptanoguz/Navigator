// navigator/mime.hpp
//
// Purpose: pure (no I/O) MIME guessing from a name or a listed entry, and the translatable
//          label shown in the Details view's Type column. platform::mime_type(path) is the I/O
//          wrapper (stat + guess_for_entry).
// Owner:   contracts (header); implementation in src/platform (role: platform). Phase 2.
//          English/Turkish texts for the label keys live in the i18n catalogs (role: ui).
// Thread-safety: every function is pure, allocation-free and noexcept; call from any thread,
//                including the UI thread for visible rows only.
//
// Name table (Phase 2 contract; tests check every row). Matching uses the suffix after the last
// '.', as fs::extension() defines it, compared ASCII-case-insensitively, except the compound
// tar suffixes which are checked first on the last two dot-separated parts. MIME strings follow
// freedesktop shared-mime-info names.
//
//   Extensions    MIME                                             Label key
//   png           image/png                                        type.image_png
//   jpg jpeg jpe  image/jpeg                                       type.image_jpeg
//   gif           image/gif                                        type.image_gif
//   webp          image/webp                                       type.image_webp
//   svg           image/svg+xml                                    type.image_svg
//   bmp           image/bmp                                        type.image
//   ico           image/vnd.microsoft.icon                         type.image
//   tif tiff      image/tiff                                       type.image
//   avif          image/avif                                       type.image
//   heic heif     image/heif                                       type.image
//   txt           text/plain                                       type.text
//   log           text/x-log                                       type.text
//   md markdown   text/markdown                                    type.markdown
//   csv           text/csv                                         type.csv
//   html htm      text/html                                        type.html
//   css           text/css                                         type.source
//   c             text/x-csrc                                      type.source
//   h             text/x-chdr                                      type.source
//   cpp cc cxx    text/x-c++src                                    type.source
//   hpp hh hxx    text/x-c++hdr                                    type.source
//   py            text/x-python                                    type.source
//   rs            text/rust                                        type.source
//   go            text/x-go                                        type.source
//   java          text/x-java                                      type.source
//   js mjs        text/javascript                                  type.source
//   sh bash       application/x-shellscript                        type.script
//   json          application/json                                 type.json
//   xml           application/xml                                  type.xml
//   yaml yml      application/yaml                                 type.text
//   toml          application/toml                                 type.text
//   pdf           application/pdf                                  type.pdf
//   zip           application/zip                                  type.archive_zip
//   tar           application/x-tar                                type.archive
//   tar.gz tgz    application/x-compressed-tar                     type.archive
//   tar.bz2       application/x-bzip-compressed-tar                type.archive
//   tar.xz        application/x-xz-compressed-tar                  type.archive
//   tar.zst       application/x-zstd-compressed-tar                type.archive
//   gz            application/gzip                                 type.archive
//   bz2           application/x-bzip2                              type.archive
//   xz            application/x-xz                                 type.archive
//   zst           application/zstd                                 type.archive
//   7z            application/x-7z-compressed                      type.archive
//   rar           application/vnd.rar                              type.archive
//   deb           application/vnd.debian.binary-package            type.package_deb
//   rpm           application/x-rpm                                type.package
//   appimage      application/vnd.appimage                         type.appimage
//   iso           application/x-cd-image                           type.disk_image
//   desktop       application/x-desktop                            type.desktop_entry
//   odt           application/vnd.oasis.opendocument.text          type.document
//   doc           application/msword                               type.document
//   docx          <ooxml>.wordprocessingml.document                type.document
//   ods           application/vnd.oasis.opendocument.spreadsheet   type.spreadsheet
//   xls           application/vnd.ms-excel                         type.spreadsheet
//   xlsx          <ooxml>.spreadsheetml.sheet                      type.spreadsheet
//   odp           application/vnd.oasis.opendocument.presentation  type.presentation
//   ppt           application/vnd.ms-powerpoint                    type.presentation
//   pptx          <ooxml>.presentationml.presentation              type.presentation
//   mp3           audio/mpeg                                       type.audio
//   flac          audio/flac                                       type.audio
//   ogg oga       audio/ogg                                        type.audio
//   opus          audio/x-opus+ogg                                 type.audio
//   wav           audio/x-wav                                      type.audio
//   m4a           audio/mp4                                        type.audio
//   mp4 m4v       video/mp4                                        type.video
//   mkv           video/x-matroska                                 type.video
//   webm          video/webm                                       type.video
//   avi           video/x-msvideo                                  type.video
//   mov           video/quicktime                                  type.video
//   3gp           video/3gpp                                       type.video
//   mts m2ts      video/mp2t                                       type.video
//   mpg mpeg      video/mpeg                                       type.video
//   ogv           video/ogg                                        type.video
//   wmv           video/x-ms-wmv                                   type.video
//   flv           video/x-flv                                      type.video
//   ttf           font/ttf                                         type.font
//   otf           font/otf                                         type.font
//   woff2         font/woff2                                       type.font
//
//   <ooxml> = application/vnd.openxmlformats-officedocument
//   Note: "ts" is ambiguous (TypeScript, MPEG transport stream, Qt translation) and is left
//   unknown in Phase 2. Every other unlisted name is "application/octet-stream".
//
// Label keys and their English text (Turkish in the catalog). The ui must provide all of them:
//   type.folder "Folder"                          type.link "Link"
//   type.link_broken "Broken link"                type.fifo "Pipe"
//   type.socket "Socket"                          type.block_device "Block device"
//   type.char_device "Character device"           type.executable "Program"
//   type.file "File"                              type.file_ext "{ext} file"
//   type.image "Image"                            type.image_png "PNG image"
//   type.image_jpeg "JPEG image"                  type.image_gif "GIF image"
//   type.image_webp "WebP image"                  type.image_svg "SVG image"
//   type.text "Text document"                     type.markdown "Markdown document"
//   type.csv "CSV document"                       type.html "HTML document"
//   type.source "Source code"                     type.script "Shell script"
//   type.json "JSON document"                     type.xml "XML document"
//   type.pdf "PDF document"                       type.archive "Archive"
//   type.archive_zip "ZIP archive"                type.package "Package"
//   type.package_deb "Debian package"             type.appimage "AppImage application"
//   type.disk_image "Disk image"                  type.desktop_entry "Desktop entry"
//   type.document "Document"                      type.spreadsheet "Spreadsheet"
//   type.presentation "Presentation"              type.audio "Audio"
//   type.video "Video"                            type.font "Font"
#pragma once

#include <string_view>

#include <navigator/fs.hpp>

namespace navigator::mime {

inline constexpr std::string_view kOctetStream = "application/octet-stream";
inline constexpr std::string_view kDirectory = "inode/directory";
inline constexpr std::string_view kSymlink = "inode/symlink"; // only for unresolvable links
inline constexpr std::string_view kFifo = "inode/fifo";
inline constexpr std::string_view kSocket = "inode/socket";
inline constexpr std::string_view kBlockDevice = "inode/blockdevice";
inline constexpr std::string_view kCharDevice = "inode/chardevice";
inline constexpr std::string_view kExecutable = "application/x-executable";

// MIME type from the name table above; kOctetStream when the name has no listed suffix.
// The result has static storage duration.
[[nodiscard]] std::string_view guess_from_name(std::string_view name) noexcept;

// MIME type of a listed entry, from its metadata only (no I/O). Static storage duration.
//   is_directory_like(entry)                          -> kDirectory
//   Symlink, dangling or target_kind Unknown          -> kSymlink
//   Symlink to a File (or Other)                      -> as a File / Other with the link's name
//   Other: by entry.mode & S_IFMT                     -> kFifo, kSocket, kBlockDevice,
//                                                        kCharDevice (anything else: kOctetStream)
//   File: guess_from_name(entry.name); if that is kOctetStream, the name has no extension and
//         any execute bit (0111) is set in entry.mode -> kExecutable
//   Unknown kind (metadata_error, DT_UNKNOWN)         -> guess_from_name(entry.name)
[[nodiscard]] std::string_view guess_for_entry(const fs::DirEntry& entry) noexcept;

// i18n key of the Type-column label for `mime` (static storage duration):
//   - the label key of the name table row (or of the inode / executable values above:
//     type.folder, type.link (kSymlink), type.fifo, type.socket, type.block_device,
//     type.char_device, type.executable);
//   - otherwise by top-level type: "image/" -> type.image, "audio/" -> type.audio,
//     "video/" -> type.video, "text/" -> type.text, "font/" -> type.font;
//   - otherwise type.file.
// type.link_broken and type.file_ext are not returned here: the ui chooses type.link_broken for
// a dangling symlink (entry.dangling) and may show type.file_ext, with "{ext}" replaced by the
// upper-cased fs::extension(name), for a type.file entry that has an extension.
[[nodiscard]] std::string_view type_label_key(std::string_view mime) noexcept;

} // namespace navigator::mime
