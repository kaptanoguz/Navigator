// tests/core/places_test_util.hpp: fake volume tables and timestamps for the places tests.
#pragma once

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <navigator/types.hpp>
#include <navigator/volume.hpp>

namespace navigator::test {

[[nodiscard]] inline Timestamp at_seconds(long long seconds) {
    return Timestamp{std::chrono::seconds{seconds}};
}

// *value, failing the test (exception) when empty; keeps optional access checked for tidy.
template <class T> [[nodiscard]] const T& must(const std::optional<T>& value) {
    if (!value.has_value()) {
        throw std::logic_error("unexpected empty optional");
    }
    return *value;
}

[[nodiscard]] inline VolumeInfo volume(Path mount_point, std::string device,
                                       std::optional<std::string> uuid, VolumeKind kind) {
    VolumeInfo v;
    v.mount_point = std::move(mount_point);
    v.device = std::move(device);
    v.fs_type = "ext4";
    v.uuid = std::move(uuid);
    v.root = "/";
    v.kind = kind;
    return v;
}

[[nodiscard]] inline VolumeInfo stick(Path mount_point) {
    VolumeInfo v = volume(std::move(mount_point), "/dev/sdb1", std::string("1234-ABCD"),
                          VolumeKind::Removable);
    v.fs_type = "vfat";
    v.label = "STICK";
    v.removable = true;
    return v;
}

// [0] "/" (Root), [1] "/home/u" (Home), [2] "/media/u/STICK" (Removable, uuid 1234-ABCD).
[[nodiscard]] inline std::vector<VolumeInfo> system_volumes() {
    return {volume("/", "/dev/nvme0n1p2", std::string("ROOT-UUID"), VolumeKind::Root),
            volume("/home/u", "/dev/nvme0n1p3", std::string("HOME-UUID"), VolumeKind::Home),
            stick("/media/u/STICK")};
}

} // namespace navigator::test
