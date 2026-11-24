// Shared fixtures of the Phase 7 ui tests: a FileOps over fixture directories with a real trash
// inside the fixture (never the owner's), the in-memory clipboard and a few wait helpers.
#pragma once

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <navigator/trash.hpp>

#include "ops/clipboard.hpp"
#include "ops/file_ops.hpp"
#include "ui_fixture.hpp"

namespace navigator::test {

struct OpsRig {
    TempDir t;
    std::shared_ptr<trash::ITrash> trash;
    ui::MemoryClipboard clip;
    std::unique_ptr<ui::FileOps> ops;
    double now = 0.0;

    explicit OpsRig(bool with_trash = true, std::function<bool(const Path&)> injector = {},
                    std::size_t chunk = std::size_t{1} << 20U) {
        std::filesystem::create_directories(t.path() / "trash");
        if (with_trash) {
            trash::TrashOptions o;
            o.home_trash = t.path() / "trash";
            o.use_topdir_trashes = false;
            auto r = trash::create_trash(std::move(o));
            if (r) {
                trash = *r;
            }
        }
        ui::FileOpsOptions fo;
        fo.trash = trash;
        fo.clipboard = &clip;
        fo.new_folder_name = [] { return std::string("New folder"); };
        fo.fault_injector = std::move(injector);
        fo.copy_chunk_bytes = chunk;
        fo.protected_paths.home = t.path() / "home";
        fo.protected_paths.trash_roots.push_back(t.path() / "trash");
        ops = std::make_unique<ui::FileOps>(std::move(fo));
    }

    bool settle(std::chrono::milliseconds timeout = std::chrono::milliseconds(8000)) {
        now += 10.0;
        return ops->wait_idle(timeout);
    }
    [[nodiscard]] Path p(std::string_view rel) const { return t.path() / rel; }
    [[nodiscard]] static std::string read(const Path& f) {
        std::ifstream in(f, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
    [[nodiscard]] static bool exists(const Path& f) {
        std::error_code ec;
        return std::filesystem::symlink_status(f, ec).type() !=
               std::filesystem::file_type::not_found;
    }
};

} // namespace navigator::test
