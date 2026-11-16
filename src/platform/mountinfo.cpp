// parse_mountinfo(), filesystem type sets and the escape decoders.
#include <array>
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/sysmacros.h>

#include <navigator/volume.hpp>

#include "volume_internal.hpp"

namespace navigator {

namespace platform_internal {

namespace {

constexpr bool is_octal(char c) noexcept { return c >= '0' && c <= '7'; }

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

template <class T> bool parse_number(std::string_view text, T& value) noexcept {
    if (text.empty()) {
        return false;
    }
    const char* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), end, value);
    return ec == std::errc{} && ptr == end;
}

bool has_option(std::string_view options, std::string_view wanted) noexcept {
    while (!options.empty()) {
        const std::size_t comma = options.find(',');
        const std::string_view token = options.substr(0, comma);
        if (token == wanted) {
            return true;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        options.remove_prefix(comma + 1);
    }
    return false;
}

std::vector<std::string_view> split_spaces(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t pos = 0;
    while (pos < line.size()) {
        while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
            ++pos;
        }
        if (pos >= line.size()) {
            break;
        }
        std::size_t end = pos;
        while (end < line.size() && line[end] != ' ' && line[end] != '\t') {
            ++end;
        }
        fields.push_back(line.substr(pos, end - pos));
        pos = end;
    }
    return fields;
}

bool parse_line(std::string_view line, MountEntry& entry) {
    const std::vector<std::string_view> f = split_spaces(line);
    // 0 id, 1 parent, 2 major:minor, 3 root, 4 mount point, 5 options, [6.. optional], "-",
    // fs type, source, super options.
    if (f.size() < 10) {
        return false;
    }
    std::size_t sep = 6;
    while (sep < f.size() && f[sep] != "-") {
        ++sep;
    }
    if (sep + 3 >= f.size()) {
        return false; // no " - " separator, or fewer than three fields after it
    }
    std::uint32_t mount_id = 0;
    std::uint32_t parent_id = 0;
    if (!parse_number(f[0], mount_id) || !parse_number(f[1], parent_id)) {
        return false;
    }
    const std::size_t colon = f[2].find(':');
    if (colon == std::string_view::npos) {
        return false;
    }
    unsigned int major_no = 0;
    unsigned int minor_no = 0;
    if (!parse_number(f[2].substr(0, colon), major_no) ||
        !parse_number(f[2].substr(colon + 1), minor_no)) {
        return false;
    }
    entry.mount_id = mount_id;
    entry.parent_id = parent_id;
    entry.device_number = static_cast<std::uint64_t>(makedev(major_no, minor_no));
    entry.root = decode_octal_escapes(f[3]);
    entry.mount_point = decode_octal_escapes(f[4]);
    entry.mount_options = std::string(f[5]);
    entry.fs_type = std::string(f[sep + 1]);
    entry.source = decode_octal_escapes(f[sep + 2]);
    entry.super_options = std::string(f[sep + 3]);
    entry.read_only =
        has_option(entry.mount_options, "ro") || has_option(entry.super_options, "ro");
    return true;
}

} // namespace

std::string decode_octal_escapes(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 3 < text.size() && is_octal(text[i + 1]) &&
            is_octal(text[i + 2]) && is_octal(text[i + 3])) {
            const int value =
                ((text[i + 1] - '0') << 6) | ((text[i + 2] - '0') << 3) | (text[i + 3] - '0');
            out.push_back(static_cast<char>(static_cast<unsigned char>(value & 0xFF)));
            i += 3;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::string decode_udev_escapes(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 3 < text.size() && text[i + 1] == 'x' &&
            hex_value(text[i + 2]) >= 0 && hex_value(text[i + 3]) >= 0) {
            const int value = (hex_value(text[i + 2]) << 4) | hex_value(text[i + 3]);
            out.push_back(static_cast<char>(static_cast<unsigned char>(value)));
            i += 3;
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

} // namespace platform_internal

std::vector<MountEntry> parse_mountinfo(std::string_view text) {
    std::vector<MountEntry> entries;
    try {
        while (!text.empty()) {
            const std::size_t nl = text.find('\n');
            std::string_view line = text.substr(0, nl);
            text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            MountEntry entry;
            if (platform_internal::parse_line(line, entry)) {
                entries.push_back(std::move(entry));
            }
        }
    } catch (...) {
        return entries; // out of memory: what was parsed so far (no error channel)
    }
    return entries;
}

bool is_pseudo_filesystem(std::string_view fs_type) noexcept {
    static constexpr std::array<std::string_view, 25> kPseudo{
        "proc",    "sysfs",   "devtmpfs",  "devpts",      "tmpfs",       "ramfs",    "cgroup",
        "cgroup2", "pstore",  "bpf",       "securityfs",  "debugfs",     "tracefs",  "configfs",
        "fusectl", "mqueue",  "hugetlbfs", "autofs",      "binfmt_misc", "efivarfs", "rpc_pipefs",
        "nsfs",    "overlay", "squashfs",  "fuse.portal",
    };
    if (fs_type == "fuse.gvfsd-fuse") {
        return true;
    }
    for (std::string_view t : kPseudo) {
        if (t == fs_type) {
            return true;
        }
    }
    return false;
}

bool is_network_filesystem(std::string_view fs_type) noexcept {
    static constexpr std::array<std::string_view, 12> kNetwork{
        "nfs", "nfs4", "cifs",      "smb3",  "smbfs",      "9p",
        "afs", "ceph", "glusterfs", "davfs", "fuse.sshfs", "fuse.rclone",
    };
    for (std::string_view t : kNetwork) {
        if (t == fs_type) {
            return true;
        }
    }
    return false;
}

} // namespace navigator
