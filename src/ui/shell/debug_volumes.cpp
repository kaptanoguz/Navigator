// src/ui/shell/debug_volumes.cpp — see debug_volumes.hpp.
#include "shell/debug_volumes.hpp"

#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>

namespace navigator::ui {
namespace {

class FileMountSource final : public IMountSource {
public:
    FileMountSource(std::string mountinfo, std::map<std::string, SpaceInfo> space)
        : mountinfo_(std::move(mountinfo)), space_(std::move(space)) {}

    Result<std::string> read_mountinfo() override { return mountinfo_; }

    Result<SpaceInfo> query_space(const Path& mount_point) override {
        const auto it = space_.find(mount_point.string());
        if (it == space_.end()) {
            return fail(ErrorCode::NotFound, "no space entry");
        }
        return it->second;
    }

private:
    std::string mountinfo_;
    std::map<std::string, SpaceInfo> space_;
};

} // namespace

Result<VolumeQuery> debug_volume_query(const Path& dir, const Path& home) {
    std::ifstream mi(dir / "mountinfo", std::ios::binary);
    if (!mi) {
        return fail(ErrorCode::NotFound, "no mountinfo in " + dir.string());
    }
    std::stringstream text;
    text << mi.rdbuf();
    std::map<std::string, SpaceInfo> space;
    std::ifstream sp(dir / "space");
    std::string mount;
    SpaceInfo s;
    while (sp >> mount >> s.total_bytes >> s.free_bytes >> s.available_bytes) {
        space[mount] = s;
    }
    VolumeQuery q;
    q.source = std::make_shared<FileMountSource>(text.str(), std::move(space));
    q.sysfs_root = dir / "sys";
    q.devfs_root = dir / "dev";
    q.home_dir = home;
    return q;
}

} // namespace navigator::ui
