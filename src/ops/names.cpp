// Pure helpers of navigator/ops.hpp: split_extension, stem_length, keep_both_name,
// validate_new_name, is_same_or_inside, protection_of, make_label. No I/O.
#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/fs.hpp>
#include <navigator/ops.hpp>

#include "sys.hpp"

namespace navigator::ops {
namespace {

constexpr std::size_t kMaxExtensionBytes = 16; // without the dot

[[nodiscard]] bool iequals_ascii(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto ca = static_cast<unsigned char>(a[i]);
        auto cb = static_cast<unsigned char>(b[i]);
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<unsigned char>(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<unsigned char>(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_compression_ext(std::string_view ext) noexcept {
    static constexpr std::array<std::string_view, 9> kExt{"gz",   "bz2", "xz", "zst", "lz",
                                                          "lzma", "lz4", "Z",  "br"};
    for (const std::string_view e : kExt) {
        if (iequals_ascii(ext, e)) {
            return true;
        }
    }
    return false;
}

// Components of a lexically normalized path; trailing empty component (from "a/") dropped.
[[nodiscard]] std::vector<std::string> components(const Path& p) {
    std::vector<std::string> out;
    for (const auto& c : p.lexically_normal()) {
        const std::string& s = c.native();
        if (s.empty() || s == ".") {
            continue;
        }
        out.push_back(s);
    }
    return out;
}

} // namespace

NameParts split_extension(std::string_view name, bool is_directory) noexcept {
    if (is_directory) {
        return {name, {}};
    }
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == name.size()) {
        return {name, {}};
    }
    const std::string_view ext = name.substr(dot + 1);
    if (ext.size() > kMaxExtensionBytes) {
        return {name, {}};
    }
    std::size_t split = dot;
    if (is_compression_ext(ext)) {
        const std::string_view before = name.substr(0, dot);
        // ".tar" must be a real extension of a non-empty stem (".tar.gz" stays a dot-file).
        if (before.size() > 4 && iequals_ascii(before.substr(before.size() - 4), ".tar")) {
            split = dot - 4;
        }
    }
    return {name.substr(0, split), name.substr(split)};
}

std::size_t stem_length(std::string_view name, bool is_directory) noexcept {
    return split_extension(name, is_directory).stem.size();
}

std::string keep_both_name(std::string_view name, unsigned n, bool is_directory) {
    if (n < 2) {
        n = 2;
    }
    const NameParts parts = split_extension(name, is_directory);
    std::string_view stem = parts.stem;
    // Strip an existing " (k)" suffix so repeated copies count up instead of nesting.
    if (stem.size() >= 4 && stem.back() == ')') {
        const std::size_t open = stem.rfind(" (");
        if (open != std::string_view::npos && open + 2 < stem.size() - 1) {
            bool digits = true;
            for (std::size_t i = open + 2; i + 1 < stem.size(); ++i) {
                if (stem[i] < '0' || stem[i] > '9') {
                    digits = false;
                    break;
                }
            }
            if (digits) {
                stem = stem.substr(0, open);
            }
        }
    }
    const std::string suffix = " (" + std::to_string(n) + ")";
    const std::size_t tail = suffix.size() + parts.extension.size();
    if (stem.size() + tail > kNameMaxBytes) {
        const std::size_t room = tail >= kNameMaxBytes ? 0 : kNameMaxBytes - tail;
        stem = detail::utf8_prefix(stem, room);
    }
    std::string out;
    out.reserve(stem.size() + tail);
    out += stem;
    out += suffix;
    out += parts.extension;
    return out;
}

NameError validate_new_name(std::string_view name) noexcept {
    if (name.empty()) {
        return NameError::Empty;
    }
    if (name == ".") {
        return NameError::Dot;
    }
    if (name == "..") {
        return NameError::DotDot;
    }
    if (name.find('/') != std::string_view::npos) {
        return NameError::ContainsSlash;
    }
    if (name.find('\0') != std::string_view::npos) {
        return NameError::ContainsNul;
    }
    if (name.size() > kNameMaxBytes) {
        return NameError::TooLong;
    }
    return NameError::None;
}

bool is_same_or_inside(const Path& path, const Path& ancestor) {
    if (path.is_absolute() != ancestor.is_absolute()) {
        return false;
    }
    const std::vector<std::string> p = components(path);
    const std::vector<std::string> a = components(ancestor);
    if (a.size() > p.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (p[i] != a[i]) {
            return false;
        }
    }
    return true;
}

Protection protection_of(const Path& path, const ProtectedPaths& protected_paths) {
    if (path.empty() || !path.is_absolute()) {
        return Protection::Relative;
    }
    const std::vector<std::string> comps = components(path);
    if (comps.size() == 1) { // only the root name "/"
        return Protection::Root;
    }
    if (!protected_paths.home.empty() && protected_paths.home.is_absolute()) {
        const std::vector<std::string> home = components(protected_paths.home);
        if (comps == home) {
            return Protection::Home;
        }
        if (is_same_or_inside(protected_paths.home, path)) {
            return Protection::AncestorOfHome;
        }
    }
    for (const Path& mp : protected_paths.mount_points) {
        if (!mp.empty() && mp.is_absolute() && components(mp) == comps) {
            return Protection::MountPoint;
        }
    }
    for (const Path& root : protected_paths.trash_roots) {
        if (!root.empty() && root.is_absolute() && is_same_or_inside(path, root)) {
            return Protection::Trash;
        }
    }
    return Protection::None;
}

UndoLabel make_label(const UndoRecord& record) {
    UndoLabel label;
    switch (record.kind) {
    case OpKind::Copy: label.key = "undo.label.copy"; break;
    case OpKind::Move: label.key = "undo.label.move"; break;
    case OpKind::Rename: label.key = "undo.label.rename"; break;
    case OpKind::MakeDirectory: label.key = "undo.label.make_directory"; break;
    case OpKind::CreateSymlink: label.key = "undo.label.create_symlink"; break;
    case OpKind::Trash: label.key = "undo.label.trash"; break;
    case OpKind::Restore: label.key = "undo.label.restore"; break;
    case OpKind::DeletePermanently:
    case OpKind::Undo:
    case OpKind::Redo: label.key = "undo.label.copy"; break; // never recorded
    }
    label.count = record.steps.size();
    if (!record.steps.empty()) {
        const UndoStep& s = record.steps.front();
        Path item;
        switch (s.kind) {
        case OpKind::Copy:
        case OpKind::Move:
        case OpKind::Rename:
        case OpKind::Trash: item = s.before.empty() ? s.after.path : s.before; break;
        default: item = s.after.path.empty() ? s.before : s.after.path; break;
        }
        label.name = fs::to_display_name(item.filename().native());
    }
    if (!record.destination.empty()) {
        label.folder = fs::to_display_name(record.destination.filename().native());
    }
    return label;
}

} // namespace navigator::ops
