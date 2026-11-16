// Fake IMountSource and fake sysfs/devfs trees for the volume tests.
#pragma once

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <navigator/volume.hpp>

#include "test_support.hpp"

namespace navigator::platform_test {

class FakeMountSource final : public IMountSource {
public:
    explicit FakeMountSource(std::string text) : texts_{std::move(text)} {}
    explicit FakeMountSource(std::vector<std::string> texts) : texts_(std::move(texts)) {}

    Result<std::string> read_mountinfo() override {
        ++reads;
        if (fail_next) {
            fail_next = false;
            return fail(ErrorCode::IoError, "fake read failure");
        }
        const std::string& text = texts_[std::min(index_, texts_.size() - 1)];
        return text;
    }
    Result<SpaceInfo> query_space(const Path& mount_point) override {
        ++space_queries;
        auto it = spaces.find(mount_point.native());
        if (it == spaces.end()) {
            return fail(ErrorCode::NotFound, "no fake space for " + mount_point.native());
        }
        return it->second;
    }

    // Moves to the next mountinfo text (the last one sticks).
    void advance() { ++index_; }
    void set_text(std::string text) {
        texts_.push_back(std::move(text));
        index_ = texts_.size() - 1;
    }

    std::map<std::string, SpaceInfo> spaces;
    bool fail_next = false;
    int reads = 0;
    int space_queries = 0;

private:
    std::vector<std::string> texts_;
    std::size_t index_ = 0;
};

// Builds <root>/sys and <root>/dev trees that mimic real sysfs and udev symlinks.
class FakeSystem {
public:
    FakeSystem() {
        std::filesystem::create_directories(sys() / "block");
        std::filesystem::create_directories(dev() / "disk" / "by-label");
        std::filesystem::create_directories(dev() / "disk" / "by-uuid");
    }

    [[nodiscard]] Path sys() const { return tmp_ / "sys"; }
    [[nodiscard]] Path dev() const { return tmp_ / "dev"; }
    [[nodiscard]] const TempDir& tmp() const { return tmp_; }

    // A disk at sys/devices/<device_path>/block/<disk> with sys/block/<disk> -> that dir
    // (as in real sysfs), its `removable` file and partition subdirectories.
    void add_disk(const std::string& disk, const std::string& device_path, bool removable,
                  const std::vector<std::string>& partitions) {
        const Path real = sys() / "devices" / device_path / "block" / disk;
        std::filesystem::create_directories(real);
        write_file(real / "removable", removable ? "1\n" : "0\n");
        for (const std::string& part : partitions) {
            std::filesystem::create_directories(real / part);
            write_file(real / part / "partition", "1\n");
        }
        std::filesystem::create_directory_symlink(std::filesystem::relative(real, sys() / "block"),
                                                  sys() / "block" / disk);
    }

    void add_label(const std::string& escaped_label, const std::string& device) {
        std::filesystem::create_symlink("../../" + device,
                                        dev() / "disk" / "by-label" / escaped_label);
    }
    void add_uuid(const std::string& uuid, const std::string& device) {
        std::filesystem::create_symlink("../../" + device, dev() / "disk" / "by-uuid" / uuid);
    }
    void remove_label(const std::string& escaped_label) {
        std::filesystem::remove(dev() / "disk" / "by-label" / escaped_label);
    }

    [[nodiscard]] VolumeQuery query(std::shared_ptr<IMountSource> source) const {
        VolumeQuery q;
        q.source = std::move(source);
        q.sysfs_root = sys();
        q.devfs_root = dev();
        q.home_dir = "/home";
        return q;
    }

private:
    TempDir tmp_;
};

} // namespace navigator::platform_test
