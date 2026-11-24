// The SDL clipboard (Phase 7): the formats of transfer::make_clipboard_offer go out through
// SDL_SetClipboardData and come back through SDL_GetClipboardData and transfer::read_clipboard.
// Runs on SDL's dummy video driver (SDL keeps a process-local clipboard there): the desktop
// clipboard is never touched.
#include <cstdlib>

#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>

#include <navigator/transfer.hpp>

#include "ops/sdl_clipboard.hpp"

using namespace navigator;
using namespace navigator::ui;

namespace {
struct Sdl {
    bool ok = false;
    Sdl() {
        ::setenv("SDL_VIDEODRIVER", "dummy", 1);
        ok = SDL_Init(SDL_INIT_VIDEO);
    }
    ~Sdl() { SDL_Quit(); }
    Sdl(const Sdl&) = delete;
    Sdl& operator=(const Sdl&) = delete;
    Sdl(Sdl&&) = delete;
    Sdl& operator=(Sdl&&) = delete;
};
} // namespace

TEST_CASE("The clipboard round-trips the file formats through SDL", "[clipboard][sdl]") {
    const Sdl sdl;
    if (!sdl.ok) {
        SKIP("SDL video is unavailable here");
    }
    SdlClipboard cb;
    transfer::FileClipboard clip;
    clip.action = transfer::ClipboardAction::Cut;
    clip.paths = {"/home/u/a b.txt", "/home/u/şarkı.txt"};
    auto offer = transfer::make_clipboard_offer(clip);
    REQUIRE(offer);
    cb.offer(std::move(*offer));
    CHECK(cb.owns_offer());
    const std::optional<std::string> gnome = cb.read(transfer::kMimeGnomeCopiedFiles);
    REQUIRE(gnome.has_value());
    CHECK(gnome.value_or(std::string()).starts_with("cut\nfile:///home/u/a%20b.txt"));
    CHECK(cb.read(transfer::kMimeKdeCutSelection) == std::optional<std::string>("1"));
    // transfer::read_clipboard over the SDL clipboard: the same paths and the cut action.
    const std::optional<transfer::FileClipboard> back =
        transfer::read_clipboard([&](std::string_view mime) { return cb.read(mime); });
    REQUIRE(back.has_value());
    CHECK(back.value_or(transfer::FileClipboard{}) == clip);
    // A new offer replaces the old one and keeps ownership; clearing drops it.
    transfer::FileClipboard copy;
    copy.paths = {"/x/y"};
    auto offer2 = transfer::make_clipboard_offer(copy);
    REQUIRE(offer2);
    cb.offer(std::move(*offer2));
    CHECK(cb.owns_offer());
    CHECK(cb.read(transfer::kMimeGnomeCopiedFiles).value_or(std::string()).starts_with("copy\n"));
    cb.clear_own();
    CHECK_FALSE(cb.owns_offer());
    CHECK_FALSE(cb.read(transfer::kMimeGnomeCopiedFiles).has_value());
    // Plain text from another program is not a file list.
    SDL_SetClipboardText("/just/a/path");
    CHECK_FALSE(cb.owns_offer());
    CHECK_FALSE(
        transfer::read_clipboard([&](std::string_view m) { return cb.read(m); }).has_value());
}
