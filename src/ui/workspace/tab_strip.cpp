// src/ui/workspace/tab_strip.cpp — see tab_strip.hpp.
#include "workspace/tab_strip.hpp"

#include <algorithm>
#include <cmath>

namespace navigator::ui {

float TabStripLayout::max_scroll(float available) const noexcept {
    return std::max(content - available, 0.0F);
}

TabStripLayout layout_tab_strip(const TabStripInput& in) {
    TabStripLayout out;
    const std::size_t n = in.natural.size();
    out.x.resize(n);
    out.w.resize(n);
    if (n == 0) {
        return out;
    }
    const float min_w = std::max(in.min_width, 1.0F);
    const float max_w = std::max(in.max_width, min_w);
    std::vector<float> want(n);
    for (std::size_t i = 0; i < n; ++i) {
        want[i] = std::clamp(std::ceil(in.natural[i]), min_w, max_w);
    }
    const float gaps = in.gap * static_cast<float>(n - 1);
    const float room = std::max(in.available - gaps, 0.0F);
    float total = 0.0F;
    for (const float w : want) {
        total += w;
    }
    if (total > room) {
        const float floor_total = min_w * static_cast<float>(n);
        if (floor_total >= room) {
            const float active_want = in.active < n ? want[in.active] : min_w;
            std::fill(want.begin(), want.end(), min_w);
            out.overflow = floor_total > room + 0.5F;
            if (out.overflow && in.active < n) {
                want[in.active] =
                    std::max(min_w, std::min(active_want, std::floor(in.available * 0.55F)));
            }
        } else {
            // Common cap c with sum(min(want_i, c)) == room: walk the sorted widths.
            std::vector<float> sorted = want;
            std::sort(sorted.begin(), sorted.end());
            float below = 0.0F; // sum of widths smaller than the cap
            float cap = min_w;
            for (std::size_t i = 0; i < n; ++i) {
                const auto rest = static_cast<float>(n - i);
                const float c = (room - below) / rest;
                if (c <= sorted[i]) {
                    cap = c;
                    break;
                }
                below += sorted[i];
                cap = sorted[i];
            }
            cap = std::max(std::floor(cap), min_w);
            for (float& w : want) {
                w = std::min(w, cap);
            }
        }
    }
    float x = 0.0F;
    for (std::size_t i = 0; i < n; ++i) {
        out.x[i] = x;
        out.w[i] = want[i];
        x += want[i] + (i + 1 < n ? in.gap : 0.0F);
    }
    out.content = x;
    out.scroll = std::clamp(in.scroll, 0.0F, out.max_scroll(in.available));
    if (!out.overflow) {
        out.scroll = 0.0F;
    }
    return out;
}

float scroll_to_show(const TabStripLayout& l, std::size_t index, float available,
                     float current) noexcept {
    if (index >= l.x.size()) {
        return current;
    }
    float s = current;
    const float left = l.x[index];
    const float right = l.x[index] + l.w[index];
    if (left < s) {
        s = left;
    } else if (right > s + available) {
        s = right - available;
    }
    return std::clamp(s, 0.0F, l.max_scroll(available));
}

std::size_t insertion_index(const TabStripLayout& l, float x) noexcept {
    std::size_t count = 0;
    for (std::size_t i = 0; i < l.x.size(); ++i) {
        if (l.x[i] + (0.5F * l.w[i]) < x) {
            ++count;
        }
    }
    return count;
}

} // namespace navigator::ui
