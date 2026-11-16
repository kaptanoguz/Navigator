// navigator/preview.hpp
//
// Purpose: inspector/preview provider interface (images, text, media, archives, ...).
// Owner:   contracts (header); implementation in src/preview (role: preview). STUB until the
//          preview phase.
// Thread-safety: providers are called from preview worker threads; render() must be reentrant
//                and must return promptly once `cancel` is set. Output is plain pixel/text data;
//                the UI thread uploads it to the renderer.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <navigator/types.hpp>

namespace navigator {

struct PreviewRequest {
    Path path;
    std::string mime_type; // may be empty; provider may sniff
    int max_width = 0;     // pixels; 0 = provider default
    int max_height = 0;
};

// Either RGBA8 pixels or UTF-8 text (or both empty for "nothing to show").
struct PreviewResult {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba; // width * height * 4, straight alpha, row-major
    std::string text;
};

class IPreviewProvider {
public:
    virtual ~IPreviewProvider() = default;

    // Stable provider id, e.g. "image.basic".
    [[nodiscard]] virtual std::string_view id() const noexcept = 0;
    // Preview phase. Cheap check (extension / MIME); no I/O.
    [[nodiscard]] virtual bool can_preview(const PreviewRequest& request) const noexcept = 0;
    // Preview phase. Errors: Unsupported, IoError, Cancelled, OutOfMemory.
    virtual Result<PreviewResult> render(const PreviewRequest& request,
                                         const std::atomic<bool>& cancel) = 0;
};

} // namespace navigator
